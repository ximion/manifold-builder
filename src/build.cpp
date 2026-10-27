/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "build.hpp"

#include "appstream.hpp"
#include "buildsystems/buildsystem.hpp"
#include "bundle.hpp"
#include "cores/core-system.hpp"
#include "error.hpp"
#include "checksum.hpp"
#include "image.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "sources/source.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fnmatch.h>
#include <manifold.h>
#include <ranges>
#include <set>
#include <sys/stat.h>
#include <thread>

namespace mfb
{

quill::Logger *const logBuild = log::logger("build");

namespace
{

/**
 * Paths we exclude from every image, regardless of distribution.
 *
 * The API filesystems are mounted at runtime. /etc/machine-id must be missing
 * or empty in a generic image, and the compose daemon provides the host's one anyway.
 */
const std::vector<std::string> IMAGE_EXCLUDES = {
    "^dev/",
    "^proc/",
    "^sys/",
    "^etc/machine-id$",
};

/**
 * Programs every build runs, whatever its recipe.
 */
constexpr std::string_view BUILD_TOOLS[] = {
    "bash",
    "bwrap",
    "erofsfuse",
    "find",
    "fsck.erofs",
    "getfattr",
    "mkfs.erofs",
    "mount",
    "setfattr",
    "systemd-repart",
    "umount",
    "unshare",
};

/**
 * Check if a cleanup pattern matches a path relative to the prefix.
 */
bool cleanup_matches(const std::string &pattern, const std::string &path)
{
    if (pattern.starts_with('/'))
        return fnmatch(pattern.c_str() + 1, path.c_str(), FNM_PATHNAME | FNM_LEADING_DIR) == 0;
    return std::ranges::any_of(fs::path(path), [&](const fs::path &component) {
        return fnmatch(pattern.c_str(), component.c_str(), 0) == 0;
    });
}

/**
 * Get the ISO 8601 date of a timestamp.
 */
std::string iso_date(int64_t epoch)
{
    return std::format("{:%F}", std::chrono::sys_seconds(std::chrono::seconds(epoch)));
}

/**
 * Get the path of a file in an image tree as seen inside the image.
 */
std::string image_path(const fs::path &tree, const fs::path &path)
{
    return "/" + path.lexically_relative(tree).string();
}

/**
 * Fail if there are any reported errors for this MetaInfo file, and warn about any
 * other problems. `name` is the location of the file, for output logs.
 */
void check_metainfo(const fs::path &file, const std::string &name)
{
    std::vector<std::string> errors;
    for (const auto &issue : validate_metainfo(read_file(file))) {
        if (issue.error)
            errors.push_back(issue.text);
        else
            LOG_WARNING(logBuild, "{}: {}", name, issue.text);
    }
    if (!errors.empty())
        fail("{} is not valid MetaInfo: {}", name, join(errors, ", "));
}

/**
 * How long the cache keeps stuff that we haven't used.
 */
constexpr std::chrono::days CACHE_MAX_UNUSED_AGE{20};

} // namespace

Build::Build(const Recipe &recipe, const fs::path &recipe_file, const BuildOptions &options)
    : m_recipe(recipe),
      m_recipe_file(fs::absolute(recipe_file)),
      m_recipe_dir(m_recipe_file.parent_path()),
      m_options(options)
{
}

Build::~Build() = default;

fs::path Build::run()
{
    log::header_box(std::format("Building {} {} {}", m_recipe.type, m_recipe.component, m_recipe.version));
    prepare();
    setup();
    try {
        stages();
        pack_outputs();
        auto bundle = make_images();
        cleanup_work_dir();
        // without a cache of its own, the build removed it with the work directory
        if (m_options.cache_dir) {
            auto removed = prune_cache(m_cache, CACHE_MAX_UNUSED_AGE);
            if (removed > 0)
                LOG_INFO(
                    logBuild,
                    "Removed {} entries no build used for {} days from the cache",
                    removed,
                    CACHE_MAX_UNUSED_AGE.count());
        }
        return bundle;
    } catch (...) {
        cleanup_work_dir();
        throw;
    }
}

void Build::setup()
{
    m_epoch = m_system->epoch();
    m_architecture = m_system->architecture();
    m_parts = parts_to_build(m_recipe, m_architecture);

    for (auto tool : BUILD_TOOLS)
        require_tool(tool);
    m_system->check_tools();
    m_system->check_packages(m_recipe);
    for (const auto &package : m_system->requested(m_recipe.packages)) {
        if (m_system->is_development_package(package))
            LOG_WARNING(
                logBuild,
                "{} is listed under 'packages', which go into the runtime; development packages belong under "
                "'packages-sdk'",
                package);
    }
    check_source_tools(m_parts);
    if (m_options.sign_key)
        check_sign_key(*m_options.sign_key);
    if (m_recipe.metainfo_file) {
        auto file = m_recipe_dir / *m_recipe.metainfo_file;
        check_metainfo(file, file.string());
    }
    Sandbox::check_available();

    m_build_info.source_date_epoch = m_epoch;
    m_build_info.builder = "manifold-builder " MFB_VERSION;
    m_build_info.tools["mkfs.erofs"] = tool_version({"mkfs.erofs", "-V"});
    m_build_info.tools["systemd-repart"] = tool_version({"systemd-repart", "--version"});
    bool uses_git = std::ranges::any_of(m_parts, [](const Part *part) {
        return std::ranges::contains(part->sources, "git", &Source::type);
    });
    if (uses_git)
        m_build_info.tools["git"] = tool_version({"git", "--version"});
    // only a core is made from a distribution archive
    if (m_recipe.is_core())
        m_system->describe_core(m_build_info);

    m_output = fs::absolute(m_options.output.value_or(
        fs::current_path() / bundle_file_name(m_recipe.component, m_recipe.version, m_architecture)));
    fs::create_directories(m_output.parent_path());
    prepare_work_dir();
    LOG_DEBUG(logBuild, "Work directory {}", m_work.string());
    m_sandbox.tmp_dir = m_work / "tmp";
    m_cache = fs::absolute(m_options.cache_dir.value_or(m_work / "cache"));
    LOG_DEBUG(logBuild, "Cache directory {}", m_cache.string());
}

void Build::prepare_work_dir()
{
    m_work = fs::absolute(
        m_options.work_dir.value_or(m_recipe_dir / std::format("_build-{}", m_recipe_file.stem().string())));
    std::error_code error;
    fs::create_directories(m_work, error);
    if (error && m_options.work_dir)
        fail("Cannot create the work directory {}: {}", m_work.string(), error.message());
    if (error)
        fail(
            "Cannot create the work directory {}: {} (use --work-dir to put it elsewhere)",
            m_work.string(),
            error.message());

    m_work_lock = FileLock::try_lock(m_work);
    if (!m_work_lock)
        fail("The work directory {} is in use by another build", m_work.string());

    auto gitignore = m_work / ".gitignore";
    if (!fs::is_empty(m_work)) {
        // only our .gitignore tells us that the contents are ours to delete
        if (!is_work_dir(m_work))
            fail("The work directory {} is not empty, and not one of ours", m_work.string());
        LOG_INFO(logBuild, "Clearing the work directory of an earlier build: {}", m_work.string());
        std::vector<fs::path> entries;
        for (const auto &entry : fs::directory_iterator(m_work)) {
            if (entry.path() != gitignore)
                entries.push_back(entry.path());
        }
        // NOTE: We keep the .gitignore, so a work directory we failed to clear
        // is still recognised as ours.
        for (const auto &entry : entries) {
            m_sandbox.remove_tree(entry);
            fs::remove_all(entry);
        }
    }
    // keeps the work directory out of git when it is inside a repository
    write_file(gitignore, WORK_DIR_GITIGNORE_DATA);
}

void Build::cleanup_work_dir()
{
    if (m_options.keep_work) {
        LOG_INFO(logBuild, "Work directory kept at {}", m_work.string());
        return;
    }
    LOG_DEBUG(logBuild, "Removing {}", m_work.string());
    m_sandbox.remove_tree(m_work);
    std::error_code ignored;
    fs::remove_all(m_work, ignored);
}

void Build::install_packages(
    Script &s,
    const fs::path &tree,
    const std::vector<std::string> &packages,
    std::string step) const
{
    if (packages.empty())
        return;
    auto cache = m_cache / "packages";
    auto mountpoint = tree / m_system->package_cache_dir();
    s.run({"mkdir", "-p", cache.string(), mountpoint.string()});
    s.run({"mount", "--bind", cache.string(), mountpoint.string()});
    s.step(std::move(step), [&] {
        m_system->install(s, tree, packages);
    });
    s.run({"umount", mountpoint.string()});
}

std::vector<std::string> Build::runtime_packages() const
{
    return m_system->packages_request_runtime(m_recipe);
}

void Build::run_actions(Script &s, Stage stage, const fs::path &tree) const
{
    for (const auto &action : m_recipe.actions) {
        if (action.stage != stage)
            continue;
        s.comment("action: " + action.name);
        s.message(std::format("running action {}...", action.name));
        s.step(std::format("action '{}'", action.name), [&] {
            m_system->run_in_tree(s, tree, traced_script(action.run));
        });
    }
}

void Build::fetch_sources()
{
    if (m_parts.empty())
        return;
    log::section_box("Retrieving Sources");
    m_sources = mfb::fetch_sources(
        m_parts,
        m_work / "sources",
        {.scratch = m_work / "fetch", .cache = m_cache, .skip = {m_work, m_output, m_cache}});
}

std::vector<std::pair<std::string, std::string>> Build::part_environment(
    const BuildSettings &settings,
    const InstallTarget &target) const
{
    std::vector<std::pair<std::string, std::string>> env = {
        {"MANIFOLD_DEST", target.prefix},
        {"MANIFOLD_ID", m_recipe.component},
        {"MANIFOLD_ARCH", m_architecture},
        {"MANIFOLD_BUILDER_N_JOBS",
         std::to_string(m_options.jobs ? m_options.jobs : std::max(1U, std::thread::hardware_concurrency()))},
    };
    auto set = [&](std::string name, const std::string &value) {
        if (!value.empty())
            env.emplace_back(std::move(name), value);
    };
    set("CFLAGS", settings.cflags);
    set("CXXFLAGS", settings.cxxflags);
    set("CPPFLAGS", settings.cppflags);
    set("LDFLAGS", settings.ldflags);
    // put a non-/usr prefix first, so parts find what earlier parts installed there
    auto own = target.prefix == "/usr" ? std::string() : target.prefix;
    set("PATH",
        make_search_path(
            {settings.prepend_path, own.empty() ? "" : own + "/bin", "/usr/bin:/bin", settings.append_path}));
    set("LD_LIBRARY_PATH",
        make_search_path(
            {settings.prepend_ld_library_path, own.empty() ? "" : own + "/lib", settings.append_ld_library_path}));
    set("PKG_CONFIG_PATH",
        make_search_path(
            {settings.prepend_pkg_config_path,
             own.empty() ? "" : own + "/lib/pkgconfig",
             own.empty() ? "" : own + "/share/pkgconfig"}));
    for (const auto &[name, value] : settings.env)
        env.emplace_back(name, value);
    return env;
}

fs::path Build::part_stage(const Part &part) const
{
    return m_work / "parts" / part.name;
}

fs::path Build::part_sources(const Part &part) const
{
    return m_work / "sources" / part.name;
}

fs::path Build::part_file(const Part &part, std::string_view suffix) const
{
    return m_work / "parts" / (part.name + std::string(suffix));
}

PartBuild Build::part_build(const Part &part, const fs::path &root, const InstallTarget &target) const
{
    auto settings = merge_build_settings(m_recipe, part, m_architecture);
    return {
        .root = root,
        .sources = part_sources(part),
        .name = part.name,
        .subdir = part.subdir,
        .epoch = m_epoch,
        .binds = {},
        .env = part_environment(settings, target),
        .script = part_build_script(part, target, settings, part_sources(part) / part.subdir),
        .script_file = part_file(part, ".sh"),
    };
}

void Build::build_parts(Script &s, const fs::path &root, const InstallTarget &target, const fs::path &prefix_dir) const
{
    auto list_files = [&](const Part &part, std::string_view suffix) {
        s.line(
            std::format(
                "find {} -mindepth 1 ! -type d -printf '%P\\n' | sort > {}",
                shell_quote(prefix_dir.string()),
                shell_quote(part_file(part, suffix).string())));
    };

    s.run({"mkdir", "-p", (m_work / "parts").string()});
    for (const auto *part : m_parts) {
        s.comment("part: " + part->name);
        s.message(std::format("Building part {}...", part->name));
        auto build = part_build(*part, root, target);
        build.binds.emplace_back(prefix_dir, target.prefix);
        build.script += shell_lines(part->post_install);
        list_files(*part, ".before");
        s.step(std::format("part '{}'", part->name), [&] {
            run_part_build(s, build);
        });
        list_files(*part, ".after");
    }
}

void Build::stage_parts(Script &s, const std::vector<fs::path> &lowers) const
{
    const InstallTarget target = {"/usr", "lib/" + m_system->multiarch()};
    auto root = m_work / "parts-root";
    // the stages of the parts built so far, the latest on top
    std::vector<fs::path> stages;
    for (const auto *part : m_parts) {
        s.comment("part: " + part->name);
        s.message(std::format("Building part {}...", part->name));
        auto layers = stages;
        layers.insert(layers.end(), lowers.begin(), lowers.end());
        s.run({"mkdir", "-p", part_stage(*part).string()});
        mount_overlay(s, layers, root);

        auto build = part_build(*part, root, target);
        build.binds.emplace_back(part_stage(*part), "/run/build/_install");
        build.env.emplace_back("DESTDIR", "/run/build/_install");
        s.step(std::format("part '{}'", part->name), [&] {
            run_part_build(s, build);
        });
        merge_into_usr(s, part_stage(*part));

        s.run({"umount", root.string()});
        stages.insert(stages.begin(), part_stage(*part));
    }
}

void Build::cleanup_stages() const
{
    for (const auto *part : m_parts) {
        auto prefix = part_stage(*part) / "usr";
        std::vector<std::string> files;
        if (fs::is_directory(prefix)) {
            for (const auto &path : list_tree(prefix)) {
                if (!fs::is_directory(fs::symlink_status(prefix / path)))
                    files.push_back(path.string());
            }
        }
        cleanup_part(*part, prefix, files);
    }
}

void Build::install_stages(Script &s, const fs::path &tree) const
{
    for (const auto *part : m_parts) {
        s.run({"cp", "-a", (part_stage(*part) / ".").string(), (tree / ".").string()});
        if (!part->post_install.empty()) {
            s.step(std::format("Post-install of part '{}'", part->name), [&] {
                m_system->run_in_tree(s, tree, traced_script(shell_lines(part->post_install)));
            });
        }
    }
}

std::vector<std::string> Build::files_added(const Part &part) const
{
    auto before_lines = read_lines(part_file(part, ".before"));
    std::set<std::string> before(before_lines.begin(), before_lines.end());
    std::vector<std::string> added;
    for (auto &path : read_lines(part_file(part, ".after"))) {
        if (!before.contains(path))
            added.push_back(std::move(path));
    }
    return added;
}

void Build::cleanup_part(const Part &part, const fs::path &root, const std::vector<std::string> &files) const
{
    for (const auto &path : files) {
        bool matches = std::ranges::any_of(part.cleanup, [&](const std::string &pattern) {
            return cleanup_matches(pattern, path);
        });
        if (!matches)
            continue;
        LOG_DEBUG(logBuild, "Cleanup of {}: {}", part.name, path);
        std::error_code error;
        fs::remove(root / path, error);
        if (error)
            fail("Can't remove {}: {}", (root / path).string(), error.message());
        for (auto dir = (root / path).parent_path(); dir != root && fs::is_empty(dir, error) && !error;
             dir = dir.parent_path())
            fs::remove(dir, error);
    }
}

Manifest Build::manifest_for(
    std::string component,
    std::string kind,
    std::string version,
    bool sdk,
    const InstalledPackages &packages) const
{
    Manifest manifest;
    manifest.component = std::move(component);
    manifest.kind = std::move(kind);
    manifest.version = std::move(version);
    manifest.revision = build_revision_from_time(m_epoch);
    manifest.architecture = m_architecture;
    manifest.sdk = sdk;
    manifest.build = m_build_info;
    m_system->set_packages(manifest, packages);
    return manifest;
}

std::vector<std::string> Build::source_lines() const
{
    std::vector<std::string> lines;
    for (const auto &source : m_sources)
        lines.push_back(std::format("source: {} {}", source.part, source.pin()));
    return lines;
}

MetaInfo Build::metainfo(const Manifest &manifest) const
{
    return MetaInfo{
        .id = manifest.component,
        .kind = "runtime",
        .name = m_recipe.name,
        .summary = m_recipe.summary,
        .description = m_recipe.description,
        .project_license = m_recipe.project_license,
        .metadata_license = m_recipe.metadata_license,
        .homepage = m_recipe.homepage,
        .version = manifest.version,
        .release_date = iso_date(m_epoch),
    };
}

void Build::pack_outputs()
{
    // put a MetaInfo file into every tree, where Manifold looks for it
    Script place;
    std::map<std::string, fs::path> placed;
    for (const auto &output : m_outputs) {
        auto source = place_metainfo(place, output);
        if (source.empty()) {
            source = generate_metainfo_file(output, placed);
            place.run({"install", "-D", "-m", "0644", source.string(), tree_metainfo(output).string()});
        }
        placed[output.manifest.component] = source;
    }
    m_sandbox.run(place);

    // only the catalog in the bundle names the images to install
    AppStreamCatalog catalog(m_work / MF_BUNDLE_CATALOG_DIR);
    Script s;
    for (const auto &output : m_outputs) {
        const auto &manifest = output.manifest;
        auto custom = manifold_custom(manifest.architecture, manifest.component, manifest.version);
        catalog.add_tree(output.tree, manifest.kind == "app" ? "/app" : "/usr", manifest.component, custom);
        install_manifest(s, output);
        pack(s, output);
    }
    catalog.write();
    m_sandbox.run(s);
}

fs::path Build::place_metainfo(Script &s, const Output &output) const
{
    const auto &id = output.manifest.component;
    auto target = tree_metainfo(output);

    // what the tree ships, the file we prefer first
    std::vector<fs::path> candidates;
    if (output.manifest.kind == "app")
        candidates = {target, target.parent_path() / (id + ".appdata.xml")};
    else
        candidates = {output.tree / "usr/local/share/metainfo" / (id + ".metainfo.xml"), target};
    std::vector<fs::path> shipped;
    for (const auto &path : candidates) {
        auto status = fs::symlink_status(path);
        if (fs::is_symlink(status))
            fail("{} must be a regular file, not a symbolic link", image_path(output.tree, path));
        if (fs::is_regular_file(status))
            shipped.push_back(path);
    }

    if (!output.manifest.sdk && id == m_recipe.component && m_recipe.metainfo_file) {
        auto source = m_recipe_dir / *m_recipe.metainfo_file;
        for (const auto &path : shipped) {
            LOG_WARNING(logBuild, "{}: the recipe's metainfo-file replaces {}", id, image_path(output.tree, path));
            s.run({"rm", path.string()});
        }
        s.run({"install", "-D", "-m", "0644", source.string(), target.string()});
        return source;
    }
    if (shipped.empty())
        return {};

    auto source = shipped.front();
    check_metainfo(source, std::format("{} {}", id, image_path(output.tree, source)));
    for (const auto &path : shipped | std::views::drop(1)) {
        LOG_WARNING(
            logBuild,
            "{}: Using {} and removing {}",
            id,
            image_path(output.tree, source),
            image_path(output.tree, path));
        // the move replaces the target
        if (path != target)
            s.run({"rm", path.string()});
    }
    if (source != target) {
        LOG_INFO(logBuild, "{}: Moving {} to {}", id, image_path(output.tree, source), image_path(output.tree, target));
        s.run({"mkdir", "-p", target.parent_path().string()});
        s.run({"mv", "-f", source.string(), target.string()});
    }
    return source;
}

fs::path Build::generate_metainfo_file(const Output &output, const std::map<std::string, fs::path> &placed) const
{
    const auto &manifest = output.manifest;
    if (manifest.kind == "app" && !manifest.sdk)
        fail(
            "{} installs no MetaInfo file: it needs `/app/share/metainfo/{}.metainfo.xml`, or a 'metainfo-file' "
            "directive in the recipe",
            manifest.component,
            manifest.component);

    MetaInfo info;
    if (manifest.sdk) {
        // NOTE: SDKs are registered after the image they belong to, so we must know its MetaInfo already!
        auto runtime_id = manifest.component.substr(0, manifest.component.rfind(".sdk"));
        auto runtime = placed.find(runtime_id);
        if (runtime == placed.end())
            fail("internal: The MetaInfo of {} is not known yet", runtime_id);
        info = sdk_metainfo(manifest, runtime_id, parse_metainfo(read_file(runtime->second)));
    } else {
        info = metainfo(manifest);
        if (info.name.empty() || info.summary.empty())
            fail(
                "{} ships no MetaInfo file: The recipe needs 'name' and 'summary' to generate one, or a "
                "'metainfo-file'",
                manifest.component);
    }

    auto file = m_work / (output.stem + ".generated.metainfo.xml");
    write_file(file, generate_metainfo(info));
    return file;
}

MetaInfo Build::sdk_metainfo(const Manifest &manifest, const std::string &runtime_id, const MetaInfo &runtime) const
{
    // an app's SDK sits on the kit SDK, but belongs to the app
    return MetaInfo{
        .id = manifest.component,
        .kind = "addon",
        .extends = runtime_id,
        .name = runtime.name + " (SDK)",
        .summary = "Development files for " + runtime.name,
        .project_license = runtime.project_license,
        .metadata_license = runtime.metadata_license,
        .homepage = runtime.homepage,
        .version = manifest.version,
        .release_date = iso_date(m_epoch),
    };
}

fs::path Build::tree_metainfo(const Output &output) const
{
    // app images ship nothing outside /app
    auto dir = output.manifest.kind == "app" ? output.tree / "app/share/metainfo" : output.tree / "usr/share/metainfo";
    return dir / (output.manifest.component + ".metainfo.xml");
}

void Build::install_manifest(Script &s, const Output &output) const
{
    // app images ship nothing outside /app, so the manifest goes there too
    auto root = output.manifest.kind == "app" ? output.tree / "app" : output.tree;
    auto json = m_work / (output.stem + ".manifold.json");
    write_manifest(output.manifest, json);
    s.run({"install", "-D", "-m", "0644", json.string(), (root / "manifold.json").string()});
}

void Build::pack(Script &s, const Output &output) const
{
    m_system->normalize(s, output.tree);
    // images are mounted nosuid, so these bits do nothing
    s.line(std::format("find {} -perm /6000 -exec chmod ug-s {{}} +", shell_quote(output.tree.string())));
    auto excludes = IMAGE_EXCLUDES;
    auto more = m_system->image_excludes();
    excludes.insert(excludes.end(), more.begin(), more.end());
    s.run(mkfs_erofs_command(
        output.tree,
        m_work / image_file_name(output.manifest, ".erofs"),
        m_epoch,
        m_recipe.image,
        image_uuid(output.manifest.content_hash),
        excludes));
}

const Output &Build::output(std::string_view stem) const
{
    for (const auto &candidate : m_outputs) {
        if (candidate.stem == stem)
            return candidate;
    }
    fail("internal: Output '{}' is not registered yet", stem);
}

fs::path Build::make_images()
{
    log::section_box("Images");
    std::vector<BundleEntry> entries;
    for (const auto &output : m_outputs) {
        auto raw = m_work / image_file_name(output.manifest, ".raw");
        auto erofs = m_work / image_file_name(output.manifest, ".erofs");
        auto uuid = image_uuid(output.manifest.content_hash);
        auto layout = make_ddi(erofs, raw, m_epoch, m_architecture, uuid, m_work / raw.stem());

        // add what we only know about the finished image to the sidecar manifest
        Manifest record = output.manifest;
        record.build.timestamp = utc_now();
        auto checksums = checksum_file(raw);
        record.image = ImageFile{
            .file = raw.filename().string(),
            .size = fs::file_size(raw),
            .blake3 = checksums.blake3,
            .sha256 = checksums.sha256,
            .verity_root_hash = layout.root_hash,
            .data_offset = layout.data_offset,
            .data_size = layout.data_size,
            .hash_offset = layout.hash_offset,
            .hash_size = layout.hash_size,
            .signature_offset = layout.signature_offset,
            .signature_size = layout.signature_size,
            .erofs =
                {
                    .compression = std::format(
                        "{},level={}", m_recipe.image.compression, m_recipe.image.compression_level),
                    .uuid = uuid,
                },
        };
        auto json = m_work / image_file_name(output.manifest, ".manifold.json");
        write_manifest(record, json);
        LOG_INFO(
            logBuild,
            "{}: {:.1f} MiB, {} packages, identity {}",
            record.image->file,
            static_cast<double>(record.image->size) / (1024 * 1024),
            m_system->packages_of(record).size(),
            record.identity);

        entries.push_back({raw.filename(), raw, true});
        entries.push_back({json.filename(), json, false});
    }

    // the catalog of all images, in a stable order; icons and media are compressed already
    auto catalog_dir = m_work / MF_BUNDLE_CATALOG_DIR;
    auto catalog_files = list_tree(catalog_dir);
    std::ranges::sort(catalog_files);
    for (const auto &file : catalog_files) {
        if (fs::is_regular_file(catalog_dir / file))
            entries.push_back(
                {(MF_BUNDLE_CATALOG_DIR / file).string(), catalog_dir / file, file.extension() != ".xml"});
    }
    write_bundle(m_output, entries, m_options.sign_key);
    verify_bundle(m_output);
    return m_output;
}

size_t prune_cache(const fs::path &cache, std::chrono::seconds unused)
{
    auto limit = std::time(nullptr) - unused.count();
    auto is_unused = [&](const fs::path &path) {
        struct stat st;
        return lstat(path.c_str(), &st) == 0 && std::max(st.st_atime, st.st_mtime) < limit;
    };

    std::vector<fs::path> files;
    for (std::string_view dir : {"downloads", "packages", "packages/partial"}) {
        std::error_code error;
        for (const auto &entry : fs::directory_iterator(cache / dir, error)) {
            // apt keeps its lock next to the packages
            if (entry.is_regular_file() && entry.path().filename() != "lock" && is_unused(entry.path()))
                files.push_back(entry.path());
        }
    }
    std::vector<fs::path> mirrors;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(cache / "git", error)) {
        if (entry.is_directory() && is_unused(entry.path()))
            mirrors.push_back(entry.path());
    }

    size_t removed = 0;
    auto remove = [&](const fs::path &path) {
        std::error_code error;
        fs::remove_all(path, error);
        if (error)
            LOG_WARNING(logBuild, "Cannot remove {} from the cache: {}", path.string(), error.message());
        else
            ++removed;
    };
    for (const auto &file : files)
        remove(file);
    for (const auto &mirror : mirrors) {
        // NOTE: The lock files stay, so a build waiting for one locks the same file.
        auto name = mirror;
        if (name.extension() == ".part")
            name.replace_extension();
        try {
            // another build may have used the mirror in the meantime
            auto lock = FileLock::try_lock(name.string() + ".lock");
            if (lock && is_unused(mirror))
                remove(mirror);
        } catch (const BuildError &error) {
            LOG_WARNING(logBuild, "Cannot remove {} from the cache: {}", mirror.string(), error.what());
        }
    }
    return removed;
}

} // namespace mfb

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "recipe.hpp"

#include "buildsystems/buildsystem.hpp"
#include "cores/core-system.hpp"
#include "error.hpp"
#include "checksum.hpp"
#include "sources/source.hpp"

#include <algorithm>
#include <cctype>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>
#include <manifold.h>
#include <set>

template<>
struct glz::meta<mfb::Stage> {
    using enum mfb::Stage;
    static constexpr auto value = enumerate("runtime", runtime, "sdk", sdk);
};

template<>
struct glz::meta<mfb::ImageSettings> {
    using T = mfb::ImageSettings;
    static constexpr auto value = object("compression", &T::compression, "compression-level", &T::compression_level);
};

template<>
struct glz::meta<mfb::Source> {
    using T = mfb::Source;
    static constexpr auto value = object(
        "type",
        &T::type,
        "dest",
        &T::dest,
        "url",
        &T::url,
        "path",
        &T::path,
        "tag",
        &T::tag,
        "branch",
        &T::branch,
        "commit",
        &T::commit,
        "sha256",
        &T::sha256,
        "strip-components",
        &T::strip_components,
        "dest-filename",
        &T::dest_filename);
};

template<>
struct glz::meta<mfb::BuildSettings> {
    using T = mfb::BuildSettings;
    static constexpr auto value = object(
        "cflags",
        &T::cflags,
        "cxxflags",
        &T::cxxflags,
        "cppflags",
        &T::cppflags,
        "ldflags",
        &T::ldflags,
        "env",
        &T::env,
        "prepend-path",
        &T::prepend_path,
        "append-path",
        &T::append_path,
        "prepend-ld-library-path",
        &T::prepend_ld_library_path,
        "append-ld-library-path",
        &T::append_ld_library_path,
        "prepend-pkg-config-path",
        &T::prepend_pkg_config_path,
        "config-opts",
        &T::config_opts,
        "arch",
        &T::arch);
};

template<>
struct glz::meta<mfb::Part> {
    using T = mfb::Part;
    static constexpr auto value = object(
        "name",
        &T::name,
        "buildsystem",
        &T::buildsystem,
        "subdir",
        &T::subdir,
        "config-opts",
        &T::config_opts,
        "make-args",
        &T::make_args,
        "make-install-args",
        &T::make_install_args,
        "build-commands",
        &T::build_commands,
        "post-install",
        &T::post_install,
        "cleanup",
        &T::cleanup,
        "only-arches",
        &T::only_arches,
        "skip-arches",
        &T::skip_arches,
        "build-options",
        &T::build_options,
        "sources",
        &T::sources,
        "parts",
        &T::parts);
};

template<>
struct glz::meta<mfb::Recipe> {
    using T = mfb::Recipe;
    static constexpr auto value = object(
        "type",
        &T::type,
        "component",
        &T::component,
        "version",
        &T::version,
        "name",
        &T::name,
        "summary",
        &T::summary,
        "description",
        &T::description,
        "project-license",
        &T::project_license,
        "metadata-license",
        &T::metadata_license,
        "homepage",
        &T::homepage,
        "metainfo-file",
        &T::metainfo_file,
        "bootstrap",
        &T::bootstrap,
        "command",
        &T::command,
        "base",
        &T::base,
        "base-version",
        &T::base_version,
        "kit-strategy",
        &T::kit_strategy,
        "generate-sdk",
        &T::generate_sdk,
        "run-on-sdk",
        &T::run_on_sdk,
        "build-options",
        &T::build_options,
        "parts",
        &T::parts,
        "packages",
        &T::packages,
        "packages-sdk",
        &T::packages_sdk,
        "locales",
        &T::locales,
        "image",
        &T::image,
        "actions",
        &T::actions,
        "sdk-only",
        &T::sdk_only);
};

namespace mfb
{

namespace
{

/**
 * Check if a relative path stays inside the directory it is relative to.
 */
bool stays_below(const std::string &path)
{
    auto relative = fs::path(path);
    return relative.is_relative() && std::ranges::none_of(relative, [](const fs::path &component) {
               return component == "..";
           });
}

/**
 * Replace each included part file with the part it contains, and set the
 * directory of every part. `including` lists the files we are currently in,
 * so we can refuse cycles.
 */
void resolve_parts(std::vector<PartEntry> &entries, const fs::path &dir, std::vector<fs::path> &including)
{
    for (auto &entry : entries) {
        if (auto *include = std::get_if<std::string>(&entry)) {
            auto path = fs::weakly_canonical(dir / *include);
            if (std::ranges::contains(including, path))
                fail("{} includes itself", path.string());
            auto yaml = read_file(path);
            Part part;
            auto error = glz::read_yaml(part, yaml);
            if (error)
                fail("{}: {}", path.string(), glz::format_error(error, yaml));
            part.dir = path.parent_path();
            including.push_back(path);
            resolve_parts(part.parts, part.dir, including);
            including.pop_back();
            entry = std::move(part);
        } else {
            auto &part = std::get<Part>(entry);
            part.dir = dir;
            resolve_parts(part.parts, dir, including);
        }
    }
}

/**
 * Collect all parts, each one followed by the parts nested in it.
 */
void collect_parts(const std::vector<PartEntry> &entries, std::vector<const Part *> &parts)
{
    for (const auto &entry : entries) {
        const auto &part = std::get<Part>(entry);
        parts.push_back(&part);
        collect_parts(part.parts, parts);
    }
}

void validate_parts(const Recipe &recipe, const auto &problem)
{
    std::vector<const Part *> parts;
    collect_parts(recipe.parts, parts);
    std::set<std::string> names;
    for (const auto *part : parts) {
        // the name is used as a directory name
        if (part->name.empty() || part->name.starts_with('.') || part->name.find('/') != std::string::npos)
            problem(std::format("'{}' is not a valid part name", part->name));
        if (!names.insert(part->name).second)
            problem(std::format("There is more than one part named '{}'", part->name));
        if (!build_system_known(part->buildsystem))
            problem(
                std::format(
                    "Part '{}': 'buildsystem' must be one of {}, not '{}'",
                    part->name,
                    join(build_system_names(), ", "),
                    part->buildsystem));
        if (part->buildsystem == "simple" && part->build_commands.empty())
            problem(std::format("Part '{}' uses the simple build system, but has no build-commands", part->name));
        if (!part->subdir.empty() && !stays_below(part->subdir))
            problem(std::format("Part '{}': subdir '{}' must stay inside the sources", part->name, part->subdir));
        for (const auto &source : part->sources) {
            if (!stays_below(source.dest))
                problem(std::format("Part '{}': dest '{}' must stay inside the sources", part->name, source.dest));
            if (auto issue = check_source(source); !issue.empty())
                problem(std::format("Part '{}': {}", part->name, issue));
        }
    }
}

void validate(const Recipe &recipe, std::string_view origin, const fs::path &dir)
{
    auto problem = [&](std::string message) {
        fail("{}: {}", origin, message);
    };

    switch (mf_component_kind_from_string(recipe.type.c_str())) {
    case MF_COMPONENT_KIND_CORE:
    case MF_COMPONENT_KIND_PLATFORM:
    case MF_COMPONENT_KIND_APP:
        break;
    default:
        problem(std::format("'type' must be 'core', 'platform' or 'app', not '{}'", recipe.type));
    }
    // refuse what Manifold would not install, before we build anything
    if (!mf_component_id_is_valid(recipe.component.c_str()))
        problem(std::format("'component' must be a reverse-DNS style identifier, not '{}'", recipe.component));
    if (recipe.version.empty())
        problem("'version' is required");
    else if (!mf_version_is_valid(recipe.version.c_str()))
        problem(
            std::format(
                "'version' must be up to 128 characters of [A-Za-z0-9.+~_-], not starting with '.' or '-', and not "
                "'current'; '{}' is not",
                recipe.version));
    if (recipe.image.compression_level < 0)
        problem("'image.compression-level' must not be negative");
    if (recipe.metainfo_file && recipe.metainfo_file->empty())
        problem("'metainfo-file' must name a file");
    else if (recipe.metainfo_file && !fs::is_regular_file(dir / *recipe.metainfo_file))
        problem(std::format("The metainfo-file '{}' does not exist", (dir / *recipe.metainfo_file).string()));

    if (recipe.is_core()) {
        if (!recipe.bootstrap)
            problem("A core recipe needs a 'bootstrap' section");
        if (auto issue = check_bootstrap(*recipe.bootstrap); !issue.empty())
            problem(issue);
        if (!recipe.base.empty() || !recipe.base_version.empty())
            problem("A core recipe has no 'base' or 'base-version': it is the lowest layer");
        std::set<std::string> locales;
        for (const auto &locale : recipe.locales) {
            bool plain = !locale.empty() && std::ranges::all_of(locale, [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            });
            if (!plain)
                problem(std::format("Locale '{}' must be a plain name like de_DE; UTF-8 is implied", locale));
            if (!locales.insert(locale).second)
                problem(std::format("Locale '{}' is listed more than once", locale));
        }
    } else if (recipe.is_platform()) {
        // a platform is built on the current core and requires exactly that version
        if (!recipe.base_version.empty())
            problem("A platform recipe has no 'base-version': it is built on the exact current core");
        if (recipe.base.empty())
            problem("A platform recipe needs 'base': the core it is built on");
        if (!recipe.locales.empty())
            problem("'locales' belongs into a core recipe: a platform gets the locales of its core");
    } else {
        if (!recipe.locales.empty())
            problem("'locales' belongs into a core recipe: an app gets the locales of its core");
        // we never generate an app's MetaInfo
        const std::pair<std::string_view, const std::string *> metainfo_keys[] = {
            {"name", &recipe.name},
            {"summary", &recipe.summary},
            {"description", &recipe.description},
            {"project-license", &recipe.project_license},
            {"homepage", &recipe.homepage},
        };
        for (const auto &[key, value] : metainfo_keys) {
            if (!value->empty())
                problem(
                    std::format(
                        "'{}' is not allowed for applications: an app ships its own MetaInfo, or names it with "
                        "'metainfo-file'",
                        key));
        }
        if (recipe.base.empty() || recipe.base_version.empty())
            problem("An app recipe needs 'base' (naming a core or a platform) and 'base-version'");
        // Manifold looks the command up in /app/bin and the PATH
        if (recipe.command.empty() || recipe.command.find('/') != std::string::npos)
            problem(
                std::format(
                    "An app recipe needs 'command': the name of the program to run, without a path, not '{}'",
                    recipe.command));
        if (recipe.parts.empty())
            problem("An app recipe needs at least one entry in 'parts'");
        if (recipe.kit_strategy != "roaming" && recipe.kit_strategy != "strict")
            problem(std::format("'kit-strategy' must be 'roaming' or 'strict', not '{}'", recipe.kit_strategy));
    }
    validate_parts(recipe, problem);

    // only an app is run, has a kit and may go without an SDK
    if (recipe.is_core() || recipe.is_platform()) {
        const std::pair<std::string_view, bool> app_keys[] = {
            {"command", !recipe.command.empty()},
            {"kit-strategy", recipe.kit_strategy != "roaming"},
            {"generate-sdk", recipe.generate_sdk},
            {"run-on-sdk", recipe.run_on_sdk},
        };
        for (const auto &[key, set] : app_keys) {
            if (set)
                problem(std::format("'{}' is only allowed in an app recipe", key));
        }
    }

    // only a core starts from an empty tree, at a pinned archive state
    if (!recipe.is_core() && recipe.bootstrap)
        problem(
            "'bootstrap' belongs into a core recipe: platforms and apps are built on an existing core, from the "
            "current archive state");

    // only a core changes its tree by hand, the layers above add packages and parts
    if (!recipe.is_core() && !recipe.actions.empty())
        problem(
            "'actions' is only allowed in a core recipe: the layers above a core change their trees only with "
            "packages and parts");
    for (const auto &action : recipe.actions) {
        if (action.run.empty())
            problem(std::format("Action '{}' has nothing to run", action.name));
    }
}

void collect_to_build(
    const std::vector<PartEntry> &entries,
    std::string_view architecture,
    std::vector<const Part *> &parts)
{
    for (const auto &entry : entries) {
        const auto &part = std::get<Part>(entry);
        if (!part.builds_on(architecture))
            continue;
        collect_to_build(part.parts, architecture, parts);
        parts.push_back(&part);
    }
}

/**
 * Apply one level of build-options on top of the previous levels.
 */
void merge_into(BuildSettings &merged, const BuildSettings &level)
{
    auto append = [](std::string &flags, const std::string &more) {
        if (more.empty())
            return;
        if (!flags.empty())
            flags += ' ';
        flags += more;
    };
    auto replace = [](std::string &value, const std::string &more) {
        if (!more.empty())
            value = more;
    };
    append(merged.cflags, level.cflags);
    append(merged.cxxflags, level.cxxflags);
    append(merged.cppflags, level.cppflags);
    append(merged.ldflags, level.ldflags);
    for (const auto &[name, value] : level.env)
        merged.env[name] = value;
    replace(merged.prepend_path, level.prepend_path);
    replace(merged.append_path, level.append_path);
    replace(merged.prepend_ld_library_path, level.prepend_ld_library_path);
    replace(merged.append_ld_library_path, level.append_ld_library_path);
    replace(merged.prepend_pkg_config_path, level.prepend_pkg_config_path);
    merged.config_opts.insert(merged.config_opts.end(), level.config_opts.begin(), level.config_opts.end());
}

void merge_level(BuildSettings &merged, const std::optional<BuildSettings> &level, std::string_view architecture)
{
    if (!level)
        return;
    merge_into(merged, *level);
    if (auto arch = level->arch.find(std::string(architecture)); arch != level->arch.end())
        merge_into(merged, arch->second);
}

} // namespace

bool Part::builds_on(std::string_view architecture) const
{
    if (!only_arches.empty() && !std::ranges::contains(only_arches, architecture))
        return false;
    return !std::ranges::contains(skip_arches, architecture);
}

std::vector<const Part *> parts_to_build(const Recipe &recipe, std::string_view architecture)
{
    std::vector<const Part *> parts;
    collect_to_build(recipe.parts, architecture, parts);
    return parts;
}

BuildSettings merge_build_settings(const Recipe &recipe, const Part &part, std::string_view architecture)
{
    BuildSettings merged;
    merge_level(merged, recipe.build_options, architecture);
    merge_level(merged, part.build_options, architecture);
    return merged;
}

Recipe parse_recipe(std::string_view yaml, std::string_view origin, const fs::path &dir)
{
    Recipe recipe;
    auto error = glz::read_yaml(recipe, yaml);
    if (error)
        fail("{}: {}", origin, glz::format_error(error, yaml));

    std::vector<fs::path> including;
    resolve_parts(recipe.parts, fs::absolute(dir), including);
    validate(recipe, origin, dir);

    // we derive the recipe hash from the normalized and combined JSON representation
    // `glz::write_json` will create minified JSON for us by default (needed, we want the hash to remain stable)
    auto json = glz::write_json(recipe);
    if (!json)
        fail("{}: cannot serialise the recipe: {}", origin, glz::format_error(json.error()));
    recipe.hash = checksum_data_hex(*json, ChecksumKind::Blake3);

    return recipe;
}

Recipe load_recipe(const fs::path &path)
{
    return parse_recipe(read_file(path), path.string(), fs::absolute(path).parent_path());
}

} // namespace mfb

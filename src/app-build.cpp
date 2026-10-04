/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "app-build.hpp"

#include "appstream.hpp"
#include "buildsystems/buildsystem.hpp"
#include "base-store.hpp"
#include "cores/core-system.hpp"
#include "error.hpp"
#include "identity.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "sandbox.hpp"

#include <ctime>

namespace mfb
{

quill::Logger *const logBuild = log::logger("build");

namespace
{

/**
 * Get the epoch for all timestamps in the images.
 *
 * App builds install the current archive state, so there is no snapshot time
 * to use. We take SOURCE_DATE_EPOCH if it is set, and the build time otherwise.
 */
int64_t build_epoch()
{
    if (const char *value = std::getenv("SOURCE_DATE_EPOCH"))
        return std::stoll(value);
    return static_cast<int64_t>(std::time(nullptr));
}

/**
 * Build of an app or a platform.
 *
 * Our scripts mount the images of the core (and the platform, if there is one)
 * with FUSE and stack overlays on them. The images are made from the upper
 * directories of these overlays, installed in this order:
 *
 *   kit-sdk      [platform-sdk + platform +] core-sdk + core    <- all packages
 *   kit          [platform +] core                              <- packages the app needs
 *
 * Each install sees the package database of its own tree. The kit SDK is
 * stacked on the kit later, so we drop the files from it that the kit has
 * unchanged. It keeps its own copy of the package state.
 *
 * We build the parts on a read-only overlay of all these layers, with the app's
 * stage directory bind-mounted at /app. Installed files land there directly,
 * and writes anywhere else fail right away. The sdk-only paths of /app move to
 * the app SDK, or are dropped if there is none.
 *
 * A platform build only makes the first two layers, under the recipe's name.
 */
class AppBuild : public Build
{
public:
    AppBuild(const Recipe &recipe, const fs::path &recipe_file, const BuildOptions &options)
        : Build(recipe, recipe_file, options)
    {
    }

private:
    void prepare() override
    {
        m_stack = find_stack(m_options.runtimes_dir, m_recipe.base, m_recipe.base_version);
        if (m_recipe.is_platform() && m_stack.platform)
            fail("A platform must be built on a core, but {} is a different platform", m_recipe.base);
        m_system = make_core_system(m_stack.core.runtime, build_epoch());
        require_tool("erofsfuse");
        require_tool("setfattr");

        LOG_INFO(
            logBuild,
            "on {} {} ({}), SOURCE_DATE_EPOCH={}",
            m_stack.base().runtime.component,
            m_stack.base().runtime.version,
            m_system->architecture(),
            m_system->epoch());
    }

    void stages() override
    {
        install_sdk();
        install_kit();
        fetch_sources();
        if (m_recipe.is_platform())
            build_platform();
        else
            build_app();
        export_trees();
    }

    fs::path layer_mount(const std::string &name) const
    {
        return m_work / "layers" / name;
    }

    std::vector<std::pair<std::string, const LayerImages *>> stack_layers() const
    {
        std::vector<std::pair<std::string, const LayerImages *>> layers = {{"core", &m_stack.core}};
        if (m_stack.platform)
            layers.push_back({"platform", &*m_stack.platform});
        return layers;
    }

    /**
     * Mount the images of the core and the platform, if there is one.
     */
    void mount_stack(Script &s) const
    {
        for (const auto &[name, layer] : stack_layers()) {
            mount_image(s, layer->runtime_image, layer->runtime.image->data_offset, layer_mount(name));
            mount_image(s, layer->sdk_image, layer->sdk.image->data_offset, layer_mount(name + "-sdk"));
        }
    }

    void unmount_stack(Script &s) const
    {
        for (const auto &[name, layer] : stack_layers()) {
            s.run({"umount", layer_mount(name + "-sdk").string()});
            s.run({"umount", layer_mount(name).string()});
        }
    }

    /**
     * Get the runtime layers below the kit, top first.
     */
    std::vector<fs::path> runtime_lowers() const
    {
        std::vector<fs::path> lowers;
        if (m_stack.platform)
            lowers.push_back(layer_mount("platform"));
        lowers.push_back(layer_mount("core"));
        return lowers;
    }

    /**
     * Get the full SDK stack below the kit SDK, top first, with `above` on top.
     * We include the lower SDKs, so their development packages are not
     * installed again.
     */
    std::vector<fs::path> sdk_lowers(std::vector<fs::path> above = {}) const
    {
        auto lowers = std::move(above);
        if (m_stack.platform) {
            lowers.push_back(layer_mount("platform-sdk"));
            lowers.push_back(layer_mount("platform"));
        }
        lowers.push_back(layer_mount("core-sdk"));
        lowers.push_back(layer_mount("core"));
        return lowers;
    }

    fs::path upper(std::string_view layer) const
    {
        return m_work / layer / "upper";
    }

    fs::path merged(std::string_view layer) const
    {
        return m_work / layer / "merged";
    }

    /**
     * Get the tree the app image is made from. Its /app is the build's /app.
     */
    fs::path app_stage() const
    {
        return m_work / "stage" / "app";
    }

    fs::path app_sdk_stage() const
    {
        return m_work / "stage" / "app-sdk";
    }

    fs::path dependencies_dump() const
    {
        return m_work / "sdk-depends.txt";
    }

    /**
     * Mount the stack's images, then an overlay for `layer` over `lowers`.
     */
    void mount_layers(Script &s, std::string_view layer, std::vector<fs::path> lowers) const
    {
        mount_stack(s);
        mount_overlay(s, lowers, upper(layer), m_work / layer / "work", merged(layer));
    }

    void unmount_layers(Script &s, std::string_view layer) const
    {
        s.run({"umount", merged(layer).string()});
        unmount_stack(s);
        strip_overlay_markers(s, upper(layer));
    }

    // --- stage 1: SDK packages ----------------------------------------------

    void install_sdk()
    {
        log::section_box("SDK Packages");
        auto development = m_system->development_packages(m_recipe);
        Script s;
        mount_layers(s, "kit-sdk", sdk_lowers());
        m_system->refresh(s, merged("kit-sdk"));
        if (!development.empty()) {
            s.begin_step("Resolving the development packages");
            m_system->dump_dependencies(s, merged("kit-sdk"), development, dependencies_dump());
            s.end_step();
        }
        install_packages(s, merged("kit-sdk"), m_system->packages_request_sdk(m_recipe), "Installing SDK packages");

        // the kit goes between this layer and the lower SDKs, so it must not hide
        // their package state,even if nothing was installed
        copy_up(s, merged("kit-sdk"), m_system->package_state_files());
        unmount_layers(s, "kit-sdk");

        m_sandbox.run(s);

        m_sdk_packages = m_system->installed_packages(upper("kit-sdk"))
                             .value_or(m_system->packages_of(m_stack.base().sdk));
    }

    // --- stage 2: kit packages ----------------------------------------------

    void install_kit()
    {
        log::section_box("Kit Packages");
        auto packages = m_system->packages_request_runtime(m_recipe, dependencies_dump());
        LOG_INFO(logBuild, "Installing {} packages...", packages.size());
        Script s;
        mount_layers(s, "kit", runtime_lowers());
        m_system->refresh(s, merged("kit"));
        install_packages(s, merged("kit"), packages, "Installing packages");
        unmount_layers(s, "kit");
        m_sandbox.run(s);

        m_kit_packages = m_system->installed_packages(upper("kit"))
                             .value_or(m_system->packages_of(m_stack.base().runtime));

        // The kit SDK sits right on the kit runtime, so what both have installed shows through
        // from the kit runtime. Below /usr, packaged files keep the time of their package
        // and regenerated caches get the time of their install.
        auto removed = remove_identical_files(upper("kit-sdk"), upper("kit"), {"usr"});
        LOG_DEBUG(logBuild, "Dropped {} files from the kit SDK that the kit runtime owns", removed);
    }

    // --- stage 3: the build -----------------------------------------------------

    void build_app()
    {
        log::section_box("Build");
        Script s;
        mount_stack(s);
        // bwrap cannot create mountpoints on a read-only root, so the top
        // layer only holds the /app and /run/build mountpoints
        auto mountpoints = m_work / "app" / "mountpoints";
        s.run({"mkdir", "-p", (mountpoints / "app").string(), (mountpoints / "run/build").string()});
        mount_overlay(s, sdk_lowers({mountpoints, upper("kit-sdk"), upper("kit")}), merged("app"));
        s.run({"mkdir", "-p", (app_stage() / "app").string()});

        build_parts(s, merged("app"), {"/app", "lib"}, app_stage() / "app");
        s.run({"umount", merged("app").string()});
        unmount_stack(s);
        m_sandbox.run(s);

        // clean up last: a later part may need what an earlier one cleans up
        for (const auto *part : m_parts)
            cleanup_part(*part, app_stage() / "app", files_added(*part));

        if (!m_recipe.command.empty() && !fs::exists(app_stage() / "app/bin" / m_recipe.command))
            LOG_WARNING(logBuild, "Command '{}' was not installed to /app/bin", m_recipe.command);
    }

    /**
     * Build the parts of a platform into its /usr, like the parts of a core.
     */
    void build_platform()
    {
        if (m_parts.empty())
            return;
        log::section_box("Build");
        Script s;
        mount_stack(s);
        stage_parts(s, sdk_lowers({upper("kit-sdk"), upper("kit")}));
        unmount_stack(s);
        m_sandbox.run(s);

        cleanup_stages();
        Script install;
        mount_layers(install, "kit", runtime_lowers());
        // the SDK sees the parts through the platform layer below it
        install_stages(install, merged("kit"));
        unmount_layers(install, "kit");
        m_sandbox.run(install);
    }

    // --- stage 4: export ----------------------------------------------------------

    void export_trees()
    {
        log::section_box("Export");
        auto base_ref = m_stack.base().runtime.as_base();
        // The kit SDK only ships when something uses it later: an app that runs
        // on it, or an app SDK for building plugins. A platform's SDK always
        // ships, since apps are built on it.
        m_outputs.push_back({"kit", upper("kit"), describe_kit(false, base_ref)});
        if (m_recipe.is_platform() || m_recipe.run_on_sdk || m_recipe.generate_sdk)
            m_outputs.push_back({"kit-sdk", upper("kit-sdk"), describe_kit(true, base_ref)});
        if (!m_recipe.is_platform()) {
            m_outputs.push_back({"app", app_stage(), describe_app(false)});
            if (m_recipe.generate_sdk)
                m_outputs.push_back({"app-sdk", app_sdk_stage(), describe_app(true)});
        }

        Script s;
        s.run({"mkdir", "-p", app_sdk_stage().string()});
        // documentation goes to the kit SDK, like in a core
        move_paths(s, upper("kit"), "usr", ALWAYS_SDK_ONLY_DIRS, upper("kit-sdk"));
        if (m_recipe.is_platform()) {
            move_paths(s, upper("kit"), "usr", m_recipe.sdk_only, upper("kit-sdk"));
        } else {
            move_paths(
                s,
                app_stage(),
                "app",
                m_recipe.sdk_only,
                m_recipe.generate_sdk ? app_sdk_stage() : m_work / "stage" / "dropped");
        }
        m_sandbox.run(s);
    }

    // --- manifests ----------------------------------------------------------------

    Manifest describe_kit(bool sdk, const BaseRef &base_ref) const
    {
        const auto &packages = sdk ? m_sdk_packages : m_kit_packages;
        // A platform takes the recipe's name, and an SDK the name of its
        // counterpart plus ".sdk". A roaming kit is shared by all apps that need the same
        // packages on the same base, so we name it after its identity. A strict
        // kit is only used by its app and named after it.
        // Kits take their base's version, so a platform update makes a new kit
        // version. The revision (build time) tells kits on the same base apart.
        auto identity = identity_hash(m_recipe.base, m_architecture, packages);
        std::string component;
        if (sdk)
            component = output("kit").manifest.component + ".sdk";
        else if (m_recipe.is_platform())
            component = m_recipe.component;
        else if (m_recipe.kit_strategy == "strict")
            component = m_recipe.component + ".kit";
        else
            component = std::format("{}.{}", m_recipe.base, identity.substr(0, 16));
        auto manifest = manifest_for(
            component,
            m_recipe.is_platform() ? "platform" : "kit",
            m_recipe.is_platform() ? m_recipe.version : base_ref.version,
            sdk,
            packages);
        manifest.identity = identity;
        // like in a core, the sources of a platform's parts are content too
        std::vector<std::string> extra;
        if (m_recipe.is_platform() && !m_sources.empty()) {
            extra = source_lines();
            manifest.sources = m_sources;
        }
        manifest.content_hash = content_hash(
            m_recipe.base, m_architecture, manifest.component, manifest.version, packages, extra);
        manifest.base = sdk ? output("kit").manifest.as_base() : base_ref;
        return manifest;
    }

    Manifest describe_app(bool sdk) const
    {
        auto manifest = manifest_for(
            m_recipe.component + (sdk ? ".sdk" : ""), "app", m_recipe.version, sdk, m_kit_packages);
        // An app is one exact build and cannot be shared like a kit, so its
        // identity is its content hash, which also covers the recipe and sources.
        std::vector<std::string> extra = {"recipe: " + m_recipe.hash};
        auto sources = source_lines();
        extra.insert(extra.end(), sources.begin(), sources.end());
        if (m_recipe.metainfo_file)
            extra.push_back(
                "metainfo: " + checksum_file_hex(m_recipe_dir / *m_recipe.metainfo_file, ChecksumKind::Blake3));
        manifest.content_hash = content_hash(
            m_recipe.base, m_architecture, manifest.component, m_recipe.version, m_kit_packages, extra);
        manifest.identity = manifest.content_hash;
        manifest.base = sdk ? output("kit-sdk").manifest.as_base() : output("kit").manifest.as_base();
        manifest.build.recipe_hash = m_recipe.hash;
        manifest.app = AppInfo{m_recipe.command, m_recipe.kit_strategy, m_recipe.run_on_sdk};
        manifest.sources = m_sources;
        return manifest;
    }

    MetaInfo metainfo(const Manifest &manifest) const override
    {
        auto info = Build::metainfo(manifest);
        if (manifest.kind != "kit")
            return info;
        // a kit has no recipe of its own to describe it
        info.name = std::format("Kit {}", manifest.identity.substr(0, 8));
        info.summary = std::format(
            "{} packages on {} for applications", m_system->packages_of(manifest).size(), m_recipe.base);
        info.description.clear();
        info.project_license = m_stack.project_license;
        info.homepage = m_stack.homepage;
        return info;
    }

    Stack m_stack;
    InstalledPackages m_kit_packages;
    InstalledPackages m_sdk_packages;
};

} // namespace

fs::path build_app(const Recipe &recipe, const fs::path &recipe_file, const BuildOptions &options)
{
    if (recipe.is_core())
        fail("Recipe {} describes a core, not an application or platform", recipe.component);
    return AppBuild(recipe, recipe_file, options).run();
}

} // namespace mfb

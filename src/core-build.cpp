/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "core-build.hpp"

#include "cores/core-system.hpp"
#include "error.hpp"
#include "identity.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "sandbox.hpp"

namespace mfb
{

quill::Logger *const logBuild = log::logger("build");

namespace
{

/**
 * Build of a core. Each stage runs as one script in the namespace:
 *
 *   1. bootstrap: create the base tree at the archive state
 *   2. runtime: install the runtime packages, run the actions
 *   3. sdk: do the same for the SDK on an overlay over the runtime tree,
 *      whose upper directory becomes the SDK image, then build the parts
 *      on read-only overlays of it into staging directories
 *   4. export: copy the parts into the runtime, move sdk-only paths to the
 *      SDK, and make both images
 */
class CoreBuild : public Build
{
public:
    CoreBuild(const Recipe &recipe, const fs::path &recipe_file, const BuildOptions &options)
        : Build(recipe, recipe_file, options)
    {
    }

private:
    void prepare() override
    {
        m_system = make_core_system(*m_recipe.bootstrap);
        // only a core starts from an empty tree
        m_system->check_bootstrap_tools();
        LOG_INFO(
            logBuild,
            "{} {}, SOURCE_DATE_EPOCH={}",
            m_recipe.bootstrap->suite,
            m_system->architecture(),
            m_system->epoch());
    }

    void stages() override
    {
        fetch_sources();
        bootstrap();
        install_runtime();
        install_sdk();
        export_trees();
    }

    fs::path rootfs() const
    {
        return m_work / "rootfs";
    }

    // --- stage 1: bootstrap ------------------------------------------------

    void bootstrap()
    {
        log::section_box("Bootstrap");
        Script s;
        s.step("Bootstrapping the core", [&] {
            m_system->bootstrap(s, rootfs(), m_recipe);
        });
        m_sandbox.run(s);
        check_merged_usr(rootfs());
    }

    // --- stage 2: runtime --------------------------------------------------

    void install_runtime()
    {
        log::section_box("Runtime packages");
        auto packages = m_system->packages_request_runtime(m_recipe);
        LOG_INFO(logBuild, "Installing {} packages...", packages.size());
        Script s;
        install_packages(s, rootfs(), packages, "Installing packages");
        if (!m_recipe.locales.empty()) {
            s.step("Generating locales", [&] {
                m_system->generate_locales(s, rootfs(), m_recipe.locales);
            });
        }
        postprocess_core(s, rootfs(), m_system->multiarch());
        run_actions(s, Stage::runtime, rootfs());
        m_sandbox.run(s);

        auto installed = m_system->installed_packages(rootfs()).value_or(InstalledPackages{});
        m_outputs.push_back({"runtime", rootfs(), describe(false, installed, std::nullopt)});
    }

    // --- stage 3: sdk ------------------------------------------------------

    void install_sdk()
    {
        log::section_box("SDK packages");
        Script s;
        mount_upper(s, "sdk", {rootfs()});
        install_packages(s, merged("sdk"), m_system->packages_request_sdk(m_recipe), "Installing SDK packages");
        run_actions(s, Stage::sdk, merged("sdk"));
        unmount_upper(s, "sdk");
        // Parts need the SDK's compilers, but what they install belongs to the
        // runtime, so we stage it for now.
        stage_parts(s, {upper("sdk"), rootfs()});
        m_sandbox.run(s);

        // if nothing was installed, the upper layer has no package database
        // and the SDK has the runtime's packages
        const auto &runtime = output("runtime").manifest;
        auto installed = m_system->installed_packages(upper("sdk")).value_or(m_system->packages_of(runtime));
        m_outputs.push_back({"sdk", upper("sdk"), describe(true, installed, runtime.as_base())});
    }

    // --- stage 4: export ---------------------------------------------------

    void export_trees()
    {
        log::section_box("Export");
        cleanup_stages();

        Script s;
        // the SDK sees the parts through the runtime layer below it
        install_stages(s, rootfs());
        m_system->finalize(s, rootfs());

        // Move sdk-only paths to the SDK instead of deleting them. The package
        // manager did not install them again in the SDK layer, so it would
        // lack them otherwise.
        move_paths(s, rootfs(), "usr", ALWAYS_SDK_ONLY_DIRS, upper("sdk"));
        move_paths(s, rootfs(), "usr", m_recipe.sdk_only, upper("sdk"));
        m_sandbox.run(s);
    }

    Manifest describe(bool sdk, const InstalledPackages &packages, std::optional<BaseRef> base) const
    {
        auto manifest = manifest_for(m_recipe.component + (sdk ? ".sdk" : ""), "core", m_recipe.version, sdk, packages);
        manifest.identity = identity_hash(m_recipe.component, m_architecture, packages);
        // locales and part sources are content too
        std::vector<std::string> extra;
        for (const auto &locale : m_recipe.locales)
            extra.push_back("locale: " + locale);
        auto sources = source_lines();
        extra.insert(extra.end(), sources.begin(), sources.end());
        if (!m_sources.empty())
            manifest.sources = m_sources;
        manifest.content_hash = content_hash(
            m_recipe.component, m_architecture, manifest.component, m_recipe.version, packages, extra);
        manifest.base = std::move(base);
        return manifest;
    }
};

} // namespace

void postprocess_core(Script &s, const fs::path &tree, const std::string &multiarch)
{
    // Let the dynamic linker find libraries in /app. The compose daemon runs
    // ldconfig per assembled tree with the app mounted at /app. We create the
    // directories in the core, so ldconfig does not complain about them while
    // the core is built.
    auto ld_conf = tree / "etc/ld.so.conf.d/manifold-app.conf";
    s.run({"mkdir", "-p", ld_conf.parent_path().string(), (tree / "app/lib" / multiarch).string()});
    auto libraries = std::format(
        R"(# Written by manifold-builder: the application's libraries.
/app/lib
/app/lib/{}
)",
        multiarch);
    s.write_file(ld_conf, libraries);

    // The compose daemon shows the host's /usr, /etc and /var/cache/fontconfig
    // below /run/host. With remap-dir, fontconfig records the host's fonts under
    // the host's own paths, so we can use the host's font cache. If its format
    // differs, fontconfig falls back to ~/.cache/fontconfig, which is shared
    // through /home.
    const std::string fonts = R"(<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<!-- Written by manifold-builder: the host's fonts, as the host sees them. -->
<fontconfig>
  <remap-dir as-path="/usr/share/fonts">/run/host/usr/share/fonts</remap-dir>
  <remap-dir as-path="/usr/local/share/fonts">/run/host/usr/local/share/fonts</remap-dir>
  <cachedir>/run/host/var/cache/fontconfig</cachedir>
</fontconfig>
)";
    auto conf = tree / "etc/fonts/conf.d/50-manifold-host-fonts.conf";
    s.run({"mkdir", "-p", conf.parent_path().string()});
    s.write_file(conf, fonts);
}

void check_merged_usr(const fs::path &tree)
{
    for (std::string_view dir : {"bin", "sbin", "lib"}) {
        std::error_code error;
        auto target = fs::read_symlink(tree / dir, error);
        if (target.is_absolute())
            target = target.relative_path();
        if (error || target.lexically_normal() != fs::path("usr") / dir)
            fail("/{0} of the core is not a link to /usr/{0}, but every core must be merged-usr", dir);
    }
}

fs::path build_core(const Recipe &recipe, const fs::path &recipe_file, const BuildOptions &options)
{
    if (!recipe.is_core())
        fail("Recipe {} does not describe a core", recipe.component);
    return CoreBuild(recipe, recipe_file, options).run();
}

} // namespace mfb

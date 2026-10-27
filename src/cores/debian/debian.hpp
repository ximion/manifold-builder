/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cores/core-system.hpp"
#include "recipe.hpp"
#include "utils.hpp"

namespace mfb
{

/**
 * Core for Debian systems.
 *
 * We create the base tree with debuerreotype, install packages with apt, read
 * what is installed from dpkg, and take the archive state of a core from
 * snapshot.debian.org.
 */
class DebianSystem : public CoreSystem
{
public:
    /**
     * Create a Debian core system.
     *
     * `bootstrap` names the suite and the dpkg architecture. If it names no
     * architecture, we build for the host's. `scripts_dir` holds the
     * debuerreotype scripts.
     */
    DebianSystem(fs::path scripts_dir, Bootstrap bootstrap, int64_t epoch);

    /**
     * Create the system for building a core.
     *
     * The epoch is the snapshot time of the recipe. The debuerreotype scripts
     * come from DEBUERREOTYPE_SCRIPT_DIR, or from where Debian installs them.
     */
    static std::unique_ptr<DebianSystem> for_core(const Bootstrap &bootstrap);

    /**
     * Create the system for building on a Debian core.
     *
     * The debuerreotype scripts are found the same way as for a core build.
     */
    static std::unique_ptr<DebianSystem> for_layer(const Manifest &core, int64_t epoch);
    /**
     * Check a Debian bootstrap section.
     *
     * Returns a description of the problem, or an empty string if there is
     * none.
     */
    static std::string check_bootstrap(const Bootstrap &bootstrap);

    void check_tools() const override;
    void check_bootstrap_tools() const override;
    std::string architecture() const override;
    std::string multiarch() const override;
    int64_t epoch() const override;
    std::string package_system() const override;
    std::vector<std::string> development_patterns() const override;
    std::vector<std::string> locale_packages() const override;
    void describe_core(BuildInfo &build) const override;

    void bootstrap(Script &s, const fs::path &rootfs, const Recipe &recipe) const override;

    /**
     * Write the epoch marker the tooling expects and update the package index.
     */
    void refresh(Script &s, const fs::path &tree) const override;

    /**
     * Run dpkg-shlibdeps over every ELF file below `dir`.
     */
    void find_linked_packages(Script &s, const fs::path &tree, const std::string &dir, const fs::path &output)
        const override;
    std::vector<std::string> read_linked_packages(const fs::path &output) const override;
    void install(Script &s, const fs::path &tree, const std::vector<std::string> &packages) const override;
    void generate_locales(Script &s, const fs::path &tree, const std::vector<std::string> &locales) const override;
    void run_in_tree(Script &s, const fs::path &tree, const std::string &command) const override;
    std::optional<InstalledPackages> installed_packages(const fs::path &tree) const override;
    std::vector<std::string> package_state_files() const override;
    std::string package_cache_dir() const override;

    /**
     * Point the tree at the regular archive instead of the snapshot.
     */
    void finalize(Script &s, const fs::path &rootfs) const override;
    void normalize(Script &s, const fs::path &tree) const override;
    std::vector<std::string> image_excludes() const override;

    /**
     * Drop documentation, manual pages and message catalogs from the
     * bootstrapped tree.
     *
     * We also configure dpkg to keep them out of everything installed later.
     * The message catalogs of `locales` are kept.
     */
    void slimify(Script &s, const fs::path &rootfs, const std::vector<std::string> &locales) const;

private:
    std::string script(std::string_view name) const;
    /**
     * Run apt-get inside the tree.
     */
    void apt_get(Script &s, const fs::path &tree, std::vector<std::string> arguments) const;

    fs::path m_scripts;
    Bootstrap m_bootstrap;
    int64_t m_epoch;
};

/**
 * Translate between dpkg and Manifold architecture names.
 *
 * Manifold names an architecture by the CPU part of its multiarch tuple
 * (x86_64, aarch64), except for 32-bit ARM, which it names by the ABI like
 * dpkg does (armhf, armel). dpkg has its own names for the others (amd64).
 */
std::string manifold_architecture(const std::string &dpkg_architecture);
std::string dpkg_architecture(const std::string &manifold_architecture);

} // namespace mfb

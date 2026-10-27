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
#include <string_view>
#include <vector>

#include "manifest.hpp"
#include "recipe.hpp"
#include "utils.hpp"

namespace mfb
{

class Script;

/**
 * Package names mapped to their direct dependencies.
 *
 * We only use this to decide what goes into a runtime vs an sdk, so of a
 * group of alternatives we keep the first, and replace a virtual package by
 * its first provider.
 */

/**
 * Interface to the software distribution that provides a core.
 *
 * A backend knows how to create the base tree, how to install packages into
 * a tree and read back what is installed, and how to make a tree ready to
 * ship. Everything else about a build is the same for every distribution.
 * Backends live in cores/<distribution>/.
 *
 * Methods that take a Script append the shell lines for one step. The script
 * then runs as root in the build namespace. The other methods read what such
 * a script left behind.
 */
class CoreSystem
{
public:
    virtual ~CoreSystem() = default;

    /**
     * Fail the build if a tool that every build on this distribution needs is missing.
     */
    virtual void check_tools() const = 0;

    /**
     * Fail the build if a tool that bootstrap() needs is missing.
     */
    virtual void check_bootstrap_tools() const = 0;

    /**
     * Get the Manifold name of the architecture we build for (e.g. "x86_64").
     */
    virtual std::string architecture() const = 0;

    /**
     * Get the multiarch tuple of the architecture (e.g. "x86_64-linux-gnu"),
     * as used in library directories.
     */
    virtual std::string multiarch() const = 0;

    /**
     * Get the SOURCE_DATE_EPOCH of the build.
     *
     * For a core, this is the archive state the packages come from.
     */
    virtual int64_t epoch() const = 0;

    /**
     * Get the key in the `packages` sections of recipes and manifests that
     * this system can install (e.g. "apt").
     */
    virtual std::string package_system() const = 0;

    /**
     * Get the patterns that match the development packages of this
     * distribution, e.g. "*-dev".
     *
     * We only use them to warn about such packages in a runtime.
     */
    virtual std::vector<std::string> development_patterns() const = 0;

    /**
     * Get the packages a core needs to generate its locales.
     */
    virtual std::vector<std::string> locale_packages() const = 0;

    /**
     * Record how a core was made: distribution, suite, archive state and
     * tool versions.
     */
    virtual void describe_core(BuildInfo &build) const = 0;

    /**
     * Create the base tree of a core at `rootfs`.
     *
     * The tree must be up to date with the archive state and have the
     * recipe's `slim` setting applied. `rootfs` must be nonexistant.
     */
    virtual void bootstrap(Script &s, const fs::path &rootfs, const Recipe &recipe) const = 0;

    /**
     * Prepare a tree assembled from images for package installation.
     */
    virtual void refresh(Script &s, const fs::path &tree) const = 0;

    /**
     * Find the packages that the programs and libraries below `dir` link
     * against, and write them to `output` for read_linked_packages().
     *
     * `dir` is a path inside `tree`, the tree they were built in. Libraries
     * below `dir` itself belong to no package and are left out.
     */
    virtual void find_linked_packages(Script &s, const fs::path &tree, const std::string &dir, const fs::path &output)
        const = 0;

    /**
     * Read the packages that find_linked_packages() found.
     */
    virtual std::vector<std::string> read_linked_packages(const fs::path &output) const = 0;

    /**
     * Install packages and their dependencies into a tree.
     *
     * Removing a package fails the install. The layers of a stack only ever
     * add to the ones below them.
     */
    virtual void install(Script &s, const fs::path &tree, const std::vector<std::string> &packages) const = 0;

    /**
     * Generate the given locales, each as UTF-8.
     */
    virtual void generate_locales(Script &s, const fs::path &tree, const std::vector<std::string> &locales) const = 0;

    /**
     * Run a shell command inside the tree as root.
     */
    virtual void run_in_tree(Script &s, const fs::path &tree, const std::string &command) const = 0;

    /**
     * Read the packages installed in a tree.
     *
     * Returns nothing if the tree records no packages, e.g. a delta tree in
     * which no packages changed.
     */
    virtual std::optional<InstalledPackages> installed_packages(const fs::path &tree) const = 0;

    /**
     * Get the files in which the package manager records what is installed,
     * relative to the tree.
     */
    virtual std::vector<std::string> package_state_files() const = 0;

    /**
     * Get the directory, relative to the tree, where install() keeps the
     * packages it downloads.
     *
     * The build binds one host directory there for every install, so the
     * installs of a build download each package only once.
     */
    virtual std::string package_cache_dir() const = 0;

    /**
     * Make the tree of a core ready to ship.
     *
     * This is the last step before the image is built.
     */
    virtual void finalize(Script &s, const fs::path &rootfs) const = 0;

    /**
     * Normalize a tree for export.
     *
     * Timestamps are set to the epoch, and logs and other build residue are
     * removed. This must work on a delta tree as well as on a full one.
     */
    virtual void normalize(Script &s, const fs::path &tree) const = 0;

    /**
     * Get the volatile state to leave out of every image (package caches
     * and indices), as regular expressions over paths relative to the tree.
     */
    virtual std::vector<std::string> image_excludes() const = 0;

    /**
     * Fail the build if the recipe lists packages for a different package
     * system.
     */
    void check_packages(const Recipe &recipe) const;

    /**
     * Get the packages of this package system from a recipe's `packages`
     * or `packages-sdk`.
     */
    std::vector<std::string> requested(const PackageSet &packages) const;

    /**
     * Get the packages of this package system that a manifest lists.
     */
    InstalledPackages packages_of(const Manifest &manifest) const;

    /**
     * Set the packages of this package system in a manifest.
     */
    void set_packages(Manifest &manifest, InstalledPackages packages) const;

    /**
     * Check if a package looks like a development package of this
     * distribution.
     */
    bool is_development_package(std::string_view package) const;

    /**
     * Get the packages to install into the runtime: those listed under
     * `packages`, and those that generate a core's locales.
     */
    std::vector<std::string> packages_request_runtime(const Recipe &recipe) const;

    /**
     * Get the packages to install into the SDK: everything listed under
     * `packages` or `packages-sdk`.
     */
    std::vector<std::string> packages_request_sdk(const Recipe &recipe) const;
};

/**
 * Check the bootstrap section of a core recipe.
 *
 * Returns a description of the problem, or an empty string if there is none.
 */
std::string check_bootstrap(const Bootstrap &bootstrap);

/**
 * Create the core system for the distribution a core recipe bootstraps.
 */
std::unique_ptr<CoreSystem> make_core_system(const Bootstrap &bootstrap);

/**
 * Create the core system of a built core, for building on it at `epoch`.
 */
std::unique_ptr<CoreSystem> make_core_system(const Manifest &core, int64_t epoch);

} // namespace mfb

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "recipe.hpp"

namespace mfb
{

/**
 * Install location of a part.
 *
 * Applications install to /app with libraries in lib, cores install to /usr
 * with libraries in the multiarch directory.
 */
struct InstallTarget {
    std::string prefix;
    /** Library directory, relative to the prefix. */
    std::string libdir;
};

/**
 * Build directory of meson and cmake, inside the source tree.
 */
inline const std::string BUILD_DIR = "_mfold-build";

/**
 * Knows how to configure, build and install a part.
 *
 * The generated scripts run inside the build environment, in the part's
 * source directory, with MANIFOLD_BUILDER_N_JOBS set. They honour DESTDIR
 * from the environment.
 */
class BuildSystem
{
public:
    virtual ~BuildSystem() = default;

    /**
     * Generate the shell lines that configure and compile the part for `target`.
     * @param part the part we build for
     * @param target the install target we install into
     * @param config_opts holds the config-opts from build-options, followed by the the part's own
     * @param source_dir the directory the build runs in, as the host sees it.
     * @return
     */
    virtual std::string build(
        const Part &part,
        const InstallTarget &target,
        const std::vector<std::string> &config_opts,
        const fs::path &source_dir) const = 0;

    /**
     * Generate the shell lines that install what build() compiled.
     */
    virtual std::string install(const Part &part) const = 0;
};

/**
 * List the build systems a part may use. cmake generates for Ninja,
 * cmake-make for make.
 */
std::vector<std::string_view> build_system_names();

/**
 * Check whether `name` is a known build system.
 */
bool build_system_known(std::string_view name);

/**
 * Get the build system with the given name. Fails the build for unknown names.
 */
std::unique_ptr<BuildSystem> build_system_for(const std::string &name);

/**
 * Create a parallel make or ninja command line, followed by the recipe's
 * extra arguments.
 */
std::string parallel_command(const std::vector<std::string> &argv, const std::vector<std::string> &arguments);

/**
 * Create the install command line of a build system, followed by the part's
 * make-install-args.
 */
std::string install_command(std::vector<std::string> argv, const Part &part);

/**
 * Create the script that builds and installs a part.
 *
 * The script runs the build system's build step, the part's build-commands
 * and then the build system's install step. The caller runs it in the
 * part's subdir, and runs post-install.
 */
std::string part_build_script(
    const Part &part,
    const InstallTarget &target,
    const BuildSettings &settings,
    const fs::path &source_dir);

} // namespace mfb

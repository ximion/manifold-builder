/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "buildsystems/cmake.hpp"

#include "process.hpp"

namespace mfb
{

CMakeBuildSystem::CMakeBuildSystem(Generator generator)
    : m_generator(generator)
{
}

std::string CMakeBuildSystem::build(
    const Part &part,
    const InstallTarget &target,
    const std::vector<std::string> &config_opts,
    const fs::path &) const
{
    // GNUInstallDirs already uses /etc for a /usr prefix, so we only set the libdir
    std::vector<std::string> configure = {
        "cmake",
        "-S",
        ".",
        "-B",
        BUILD_DIR,
        "-G",
        m_generator == Generator::Ninja ? "Ninja" : "Unix Makefiles",
        "-DCMAKE_INSTALL_PREFIX=" + target.prefix,
        "-DCMAKE_INSTALL_LIBDIR=" + target.libdir,
        "-DCMAKE_BUILD_TYPE=Release",
    };
    configure.insert(configure.end(), config_opts.begin(), config_opts.end());
    return std::format(
        "{}\n{}", shell_join(configure), parallel_command({build_tool(), "-C", BUILD_DIR}, part.make_args));
}

std::string CMakeBuildSystem::install(const Part &part) const
{
    std::vector<std::string> install = {build_tool(), "-C", BUILD_DIR, "install"};
    install.insert(install.end(), part.make_install_args.begin(), part.make_install_args.end());
    return shell_join(install) + "\n";
}

std::string CMakeBuildSystem::build_tool() const
{
    return m_generator == Generator::Ninja ? "ninja" : "make";
}

} // namespace mfb

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "buildsystems/autotools.hpp"

#include "process.hpp"

#include <unistd.h>

namespace mfb
{

namespace
{

bool is_executable(const fs::path &path)
{
    return fs::is_regular_file(path) && access(path.c_str(), X_OK) == 0;
}

/**
 * Get the command that creates the configure script, if the sources lack
 * one. Like flatpak-builder, we prefer the project's own script.
 */
std::string configure_generator(const fs::path &source_dir)
{
    if (is_executable(source_dir / "configure"))
        return {};
    for (std::string_view name : {"autogen", "autogen.sh", "bootstrap", "bootstrap.sh"}) {
        if (is_executable(source_dir / name))
            return std::format("NOCONFIGURE=1 ./{}\n", name);
    }
    return "autoreconf -fi\n";
}

} // namespace

std::string AutotoolsBuildSystem::build(
    const Part &part,
    const InstallTarget &target,
    const std::vector<std::string> &config_opts,
    const fs::path &source_dir) const
{
    std::vector<std::string> configure = {
        "./configure", "--prefix=" + target.prefix, "--libdir=" + target.prefix + "/" + target.libdir};
    if (target.prefix == "/usr")
        configure.insert(configure.end(), {"--sysconfdir=/etc", "--localstatedir=/var"});
    configure.insert(configure.end(), config_opts.begin(), config_opts.end());
    return std::format(
        "{}{}\n{}", configure_generator(source_dir), shell_join(configure), parallel_command({"make"}, part.make_args));
}

std::string AutotoolsBuildSystem::install(const Part &part) const
{
    std::vector<std::string> install = {"make", "install"};
    install.insert(install.end(), part.make_install_args.begin(), part.make_install_args.end());
    return shell_join(install) + "\n";
}

} // namespace mfb

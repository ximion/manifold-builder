/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "buildsystems/meson.hpp"

#include "process.hpp"

namespace mfb
{

std::string MesonBuildSystem::build(
    const Part &part,
    const InstallTarget &target,
    const std::vector<std::string> &config_opts,
    const fs::path &) const
{
    std::vector<std::string> setup = {
        "meson",
        "setup",
        BUILD_DIR,
        "--prefix=" + target.prefix,
        "--libdir=" + target.libdir,
        "--buildtype=release",
    };
    setup.insert(setup.end(), config_opts.begin(), config_opts.end());
    return std::format("{}\n{}", shell_join(setup), parallel_command({"ninja", "-C", BUILD_DIR}, part.make_args));
}

std::string MesonBuildSystem::install(const Part &part) const
{
    std::vector<std::string> install = {"ninja", "-C", BUILD_DIR, "install"};
    install.insert(install.end(), part.make_install_args.begin(), part.make_install_args.end());
    return shell_join(install) + "\n";
}

} // namespace mfb

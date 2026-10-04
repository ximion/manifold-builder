/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "buildsystems/buildsystem.hpp"

#include "buildsystems/autotools.hpp"
#include "buildsystems/cmake.hpp"
#include "buildsystems/meson.hpp"
#include "buildsystems/simple.hpp"
#include "error.hpp"
#include "process.hpp"

#include <algorithm>

namespace mfb
{

bool build_system_known(std::string_view name)
{
    return std::ranges::contains(BUILD_SYSTEMS, name);
}

std::unique_ptr<BuildSystem> build_system_for(const std::string &name)
{
    if (name == "meson")
        return std::make_unique<MesonBuildSystem>();
    if (name == "cmake")
        return std::make_unique<CMakeBuildSystem>(CMakeBuildSystem::Generator::Ninja);
    if (name == "cmake-make")
        return std::make_unique<CMakeBuildSystem>(CMakeBuildSystem::Generator::Make);
    if (name == "autotools")
        return std::make_unique<AutotoolsBuildSystem>();
    if (name == "simple")
        return std::make_unique<SimpleBuildSystem>();
    fail("unknown build system '{}'", name);
}

std::string parallel_command(const std::vector<std::string> &argv, const std::vector<std::string> &arguments)
{
    auto line = std::format("{} -j\"$MANIFOLD_BUILDER_N_JOBS\"", shell_join(argv));
    if (!arguments.empty())
        line += " " + shell_join(arguments);
    return line + "\n";
}

std::string part_build_script(
    const Part &part,
    const InstallTarget &target,
    const BuildSettings &settings,
    const fs::path &source_dir)
{
    auto system = build_system_for(part.buildsystem);
    auto config_opts = settings.config_opts;
    config_opts.insert(config_opts.end(), part.config_opts.begin(), part.config_opts.end());

    auto script = system->build(part, target, config_opts, source_dir);
    script += shell_lines(part.build_commands);
    script += system->install(part);
    return script;
}

} // namespace mfb

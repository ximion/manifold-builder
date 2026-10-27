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

namespace
{

using BuildSystemFactory = std::unique_ptr<BuildSystem> (*)();

template<typename System>
std::unique_ptr<BuildSystem> create()
{
    return std::make_unique<System>();
}

template<CMakeBuildSystem::Generator generator>
std::unique_ptr<BuildSystem> create_cmake()
{
    return std::make_unique<CMakeBuildSystem>(generator);
}

/**
 * Build systems a part may use.
 */
const std::pair<std::string_view, BuildSystemFactory> BUILD_SYSTEMS[] = {
    {"meson", create<MesonBuildSystem>},
    {"cmake", create_cmake<CMakeBuildSystem::Generator::Ninja>},
    {"cmake-make", create_cmake<CMakeBuildSystem::Generator::Make>},
    {"autotools", create<AutotoolsBuildSystem>},
    {"simple", create<SimpleBuildSystem>},
};

} // namespace

std::vector<std::string_view> build_system_names()
{
    auto names = BUILD_SYSTEMS | std::views::keys;
    return {names.begin(), names.end()};
}

bool build_system_known(std::string_view name)
{
    return std::ranges::contains(BUILD_SYSTEMS | std::views::keys, name);
}

std::unique_ptr<BuildSystem> build_system_for(const std::string &name)
{
    for (const auto &[known, factory] : BUILD_SYSTEMS) {
        if (known == name)
            return factory();
    }
    fail("unknown build system '{}'", name);
}

std::string parallel_command(const std::vector<std::string> &argv, const std::vector<std::string> &arguments)
{
    auto line = std::format("{} -j\"$MANIFOLD_BUILDER_N_JOBS\"", shell_join(argv));
    if (!arguments.empty())
        line += " " + shell_join(arguments);
    return line + "\n";
}

std::string install_command(std::vector<std::string> argv, const Part &part)
{
    argv.insert(argv.end(), part.make_install_args.begin(), part.make_install_args.end());
    return shell_join(argv) + "\n";
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

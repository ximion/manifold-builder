/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cores/core-system.hpp"

#include "cores/debian/debian.hpp"
#include "error.hpp"
#include "manifest.hpp"

#include <algorithm>
#include <fnmatch.h>
#include <set>

namespace mfb
{

void CoreSystem::check_packages(const Recipe &recipe) const
{
    for (const auto *set : {&recipe.packages, &recipe.packages_sdk}) {
        for (const auto &[system, packages] : *set) {
            if (system != package_system() && !packages.empty())
                fail(
                    "The recipe lists {} packages, but {} is built with {}",
                    system,
                    recipe.is_core() ? recipe.component : recipe.base,
                    package_system());
        }
    }
}

std::vector<std::string> CoreSystem::requested(const PackageSet &packages) const
{
    auto found = packages.find(package_system());
    return found == packages.end() ? std::vector<std::string>{} : found->second;
}

InstalledPackages CoreSystem::packages_of(const Manifest &manifest) const
{
    auto found = manifest.packages.find(package_system());
    return found == manifest.packages.end() ? InstalledPackages{} : found->second;
}

void CoreSystem::set_packages(Manifest &manifest, InstalledPackages packages) const
{
    manifest.packages[package_system()] = std::move(packages);
}

bool CoreSystem::is_development_package(std::string_view package) const
{
    std::string name(package);
    return std::ranges::any_of(development_patterns(), [&](const std::string &pattern) {
        return fnmatch(pattern.c_str(), name.c_str(), 0) == 0;
    });
}

std::vector<std::string> CoreSystem::packages_request_runtime(const Recipe &recipe) const
{
    auto listed = requested(recipe.packages);
    std::set<std::string> runtime(listed.begin(), listed.end());
    if (!recipe.locales.empty()) {
        for (const auto &package : locale_packages())
            runtime.insert(package);
    }
    return {runtime.begin(), runtime.end()};
}

std::vector<std::string> CoreSystem::packages_request_sdk(const Recipe &recipe) const
{
    std::set<std::string> packages;
    for (const auto *set : {&recipe.packages, &recipe.packages_sdk}) {
        auto listed = requested(*set);
        packages.insert(listed.begin(), listed.end());
    }
    return {packages.begin(), packages.end()};
}

std::string check_bootstrap(const Bootstrap &bootstrap)
{
    if (bootstrap.distribution == "debian")
        return DebianSystem::check_bootstrap(bootstrap);
    return std::format(
        "only 'debian' is supported as bootstrap distribution for now, not '{}'", bootstrap.distribution);
}

std::unique_ptr<CoreSystem> make_core_system(const Bootstrap &bootstrap)
{
    if (auto problem = check_bootstrap(bootstrap); !problem.empty())
        fail("{}", problem);
    return DebianSystem::for_core(bootstrap);
}

std::unique_ptr<CoreSystem> make_core_system(const Manifest &core, int64_t epoch)
{
    // cores built before we recorded the distribution are all Debian
    auto distribution = core.build.distribution.value_or("debian");
    if (distribution != "debian")
        fail("{} is made of {}, which this builder cannot build on", core.component, distribution);
    return DebianSystem::for_layer(core, epoch);
}

} // namespace mfb

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cores/debian/dpkg.hpp"

#include "error.hpp"
#include "utils.hpp"

namespace mfb
{

InstalledPackages parse_dpkg_status(std::string_view text)
{
    InstalledPackages packages;
    std::string name;
    std::string version;
    bool installed = false;

    auto finish_paragraph = [&] {
        if (installed && !name.empty() && !version.empty())
            packages[name] = version;
        name.clear();
        version.clear();
        installed = false;
    };

    for_each_line(text, [&](std::string_view line) {
        if (trim(line).empty()) {
            finish_paragraph();
        } else if (line.starts_with("Package: ")) {
            name = trim(line.substr(9));
        } else if (line.starts_with("Version: ")) {
            version = trim(line.substr(9));
        } else if (line.starts_with("Status: ")) {
            // only "install ok installed" counts, anything else is a package
            // in a transitional state or one that left only its config files
            installed = trim(line.substr(8)).ends_with(" installed");
        }
    });
    finish_paragraph();

    return packages;
}

InstalledPackages read_dpkg_status(const fs::path &status_file)
{
    return parse_dpkg_status(read_file(status_file));
}

DependencyGraph parse_apt_cache_depends(std::string_view text)
{
    // The format:
    //
    //   qt6-base-dev                     <- a package, at column 0
    //     Depends: libqt6core6t64        <- one of its dependencies
    //    |Depends: libgl-dev             <- first of a group of alternatives;
    //     Depends: libgl1-mesa-dev          the last alternative has no bar
    //     Depends: <libgl-dev>           <- a virtual package, followed by
    //       libglvnd-dev                    the packages providing it
    DependencyGraph graph;
    std::string package;
    bool skipping_alternatives = false;
    bool expecting_provider = false;

    for_each_line(text, [&](std::string_view line) {
        if (line.empty())
            return;

        if (!std::isspace(static_cast<unsigned char>(line.front()))) {
            package = std::string(trim(line));
            graph[package];
            skipping_alternatives = false;
            expecting_provider = false;
            return;
        }

        if (line.starts_with("    ")) {
            // take the first provider of the virtual package above
            if (expecting_provider) {
                graph[package].push_back(std::string(trim(line)));
                expecting_provider = false;
            }
            return;
        }

        bool alternative_follows = line.starts_with(" |");
        auto colon = line.find(':');
        if (colon == std::string_view::npos)
            return;
        std::string_view dependency = trim(line.substr(colon + 1));
        expecting_provider = false;

        if (skipping_alternatives) {
            // we already took the first alternative of this group
            skipping_alternatives = alternative_follows;
            return;
        }
        skipping_alternatives = alternative_follows;

        if (dependency.starts_with('<')) {
            expecting_provider = true;
            return;
        }
        if (!dependency.empty())
            graph[package].push_back(std::string(dependency));
    });

    return graph;
}

} // namespace mfb

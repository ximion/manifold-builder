/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cores/debian/dpkg.hpp"

#include "error.hpp"
#include "utils.hpp"

#include <ranges>
#include <set>

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

std::vector<std::string> parse_shlibs_depends(std::string_view text)
{
    // e.g. "shlibs:Depends=libc6 (>= 2.34), libfoo1 | libfoo1t64, libbar2:any"
    constexpr std::string_view key = "shlibs:Depends=";
    std::set<std::string> packages;
    for_each_line(text, [&](std::string_view line) {
        if (!line.starts_with(key))
            return;
        line.remove_prefix(key.size());
        for (auto entry : std::views::split(line, ',')) {
            // the first alternative will do, at any version
            std::string_view dependency(entry.begin(), entry.end());
            dependency = trim(dependency.substr(0, dependency.find_first_of("|(")));
            dependency = dependency.substr(0, dependency.find(':'));
            if (!dependency.empty())
                packages.emplace(dependency);
        }
    });
    return {packages.begin(), packages.end()};
}

} // namespace mfb

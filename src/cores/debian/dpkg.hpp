/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cores/core-system.hpp"
#include "utils.hpp"

// Readers for what dpkg and apt tell us about a tree.
namespace mfb
{

/**
 * Read the packages that a dpkg status file records as installed.
 */
InstalledPackages read_dpkg_status(const fs::path &status_file);
InstalledPackages parse_dpkg_status(std::string_view text);

/**
 * Get the packages that the `shlibs:Depends` of dpkg-shlibdeps output names,
 * the first alternative of each.
 */
std::vector<std::string> parse_shlibs_depends(std::string_view text);

} // namespace mfb

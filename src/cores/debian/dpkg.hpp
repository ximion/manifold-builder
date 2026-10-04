/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <string>
#include <string_view>

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
 * Parse the dependency graph from the output of
 * `apt-cache depends --recurse --important`.
 */
DependencyGraph parse_apt_cache_depends(std::string_view text);

} // namespace mfb

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "build.hpp"
#include "recipe.hpp"
#include "utils.hpp"

namespace mfb
{

class Script;

/**
 * Add the configuration every core needs to work as a Manifold runtime.
 *
 * We run this after the packages are installed, for every distribution.
 * It e.g. makes fontconfig use the host's fonts and the dynamic linker
 * look into /app. `multiarch` is the name of the architecture's library
 * directory, e.g. x86_64-linux-gnu.
 */
void postprocess_core(Script &s, const fs::path &tree, const std::string &multiarch);

/**
 * Fail unless a core's tree is merged-usr, with /bin, /sbin and /lib
 * linking into /usr.
 */
void check_merged_usr(const fs::path &tree);

/**
 * Build the runtime and SDK images of a core into a bundle, and return its path.
 */
fs::path build_core(const Recipe &recipe, const fs::path &recipe_file, const BuildOptions &options);

} // namespace mfb

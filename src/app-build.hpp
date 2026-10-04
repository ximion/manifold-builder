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

/**
 * Build an app or a platform and return the path of its bundle.
 *
 * For an app, the bundle holds the kit, the app image, and maybe the kit SDK and
 * app SDK when the recipe needs them. For a platform, it holds the platform
 * and its SDK.
 */
fs::path build_app(const Recipe &recipe, const fs::path &recipe_file, const BuildOptions &options);

} // namespace mfb

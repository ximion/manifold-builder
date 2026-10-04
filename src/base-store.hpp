/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <optional>
#include <string>

#include "utils.hpp"
#include "identity.hpp"
#include "manifest.hpp"

namespace mfb
{

/**
 * A layer found in the store: its runtime and SDK images with their manifests.
 */
struct LayerImages {
    fs::path runtime_image;
    fs::path sdk_image;
    Manifest runtime;
    Manifest sdk;
};

/**
 * The layers a build sits on: the core, and the platform if the recipe's
 * base is one.
 */
struct Stack {
    LayerImages core;
    std::optional<LayerImages> platform;
    std::string project_license; /// from the base's MetaInfo, for the kits built on it
    std::string homepage;

    const LayerImages &base() const
    {
        return platform ? *platform : core;
    }
};

/**
 * Find the base of a build, a core or a platform, in `dir`.
 *
 * `dir` is either a Manifold store, where we take the `current` version and
 * require it to be at least `minimum_version`, or a plain directory of
 * extracted bundles, where the newest version and revision that satisfies
 * the minimum wins. A platform brings the exact core version it was built on.
 */
Stack find_stack(const fs::path &dir, const std::string &base, const std::string &minimum_version);

} // namespace mfb

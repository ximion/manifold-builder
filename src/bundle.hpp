/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "utils.hpp"

#include <manifold.h>
#include <string>
#include <vector>

namespace mfb
{

/**
 * An image, with what a bundle has to say about it.
 */
struct BundleImage {
    fs::path file;
    std::string manifest; ///< Its manifest, as JSON
    std::string catalog;  ///< AppStream catalog XML with its component
};

/**
 * Write the bundle with the cached icons and the media of its components,
 * and sign it if there is a `signer`.
 *
 * If signing fails, this fails the build, but the bundle is written unsigned.
 */
void write_bundle(
    const fs::path &bundle_path,
    const std::vector<BundleImage> &images,
    const fs::path &icons_dir,
    const fs::path &media_dir,
    MfSigner *signer);

/**
 * Find the key to sign with, and check that we can use it before there is
 * anything to sign.
 *
 * `key` is anything `manifold bundle sign --key` accepts. If it is null, we
 * use the default key, or return null if there is none. Fails the build if we
 * can not sign with the key. The caller owns the result.
 */
MfSigner *select_signer(const char *key);

/**
 * Read the bundle like Manifold does when installing it, and verify every
 * image against its manifest.
 *
 * Fails the build if Manifold would refuse the bundle.
 */
void verify_bundle(const fs::path &bundle_path);

} // namespace mfb

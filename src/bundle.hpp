/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "utils.hpp"

#include <cstdint>
#include <manifold.h>
#include <string>
#include <string_view>
#include <vector>

namespace mfb
{

constexpr std::string_view INDEX_NAME = "index.json";

struct BundleEntry {
    std::string name; ///< Name inside the archive
    fs::path source;
    bool uncompressed = false; ///< Store without compression, for images that are compressed already
};

struct IndexEntry {
    std::string name;
    uint64_t size = 0;
    std::string blake3;
    std::string sha256;
};

struct BundleIndex {
    int format = 1;
    std::vector<IndexEntry> files;
};

/**
 * Check if a file of this name belongs in index.json, which lists the
 * manifests and the files of the catalog.
 */
bool is_indexed(std::string_view name);

/**
 * Write the bundle with its index.json.
 */
void write_bundle(const fs::path &bundle_path, const std::vector<BundleEntry> &entries);

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
 * Sign a bundle in place.
 *
 * Fails the build if that is not possible, and leaves the bundle as it was.
 */
void sign_bundle(const fs::path &bundle_path, MfSigner *signer);

/**
 * Read the bundle like Manifold does when installing it, and verify every
 * image against its manifest.
 *
 * Fails the build if Manifold would refuse the bundle.
 */
void verify_bundle(const fs::path &bundle_path);

} // namespace mfb

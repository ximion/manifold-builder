/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "utils.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mfb
{

constexpr std::string_view INDEX_NAME = "index.json";
constexpr std::string_view SIGNATURE_NAME = "index.json.sig";
/// The SSHSig namespace the index is signed under.
constexpr std::string_view SIGN_NAMESPACE = "manifold-bundle";

struct BundleEntry {
    std::string name; /// Name inside the archive
    fs::path source;
    bool uncompressed = false; /// Store without compression, for images that are compressed already
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
 * Write the bundle with its index.json, and sign it with `sign_key` if given.
 *
 * The key is anything `ssh-keygen -Y sign -f` accepts: a private key, or a
 * public key whose private half is in ssh-agent.
 */
void write_bundle(
    const fs::path &bundle_path,
    const std::vector<BundleEntry> &entries,
    const std::optional<fs::path> &sign_key = std::nullopt);

/**
 * Check that we can sign with `key`, before there is anything to sign.
 */
void check_sign_key(const fs::path &key);

/**
 * Read the bundle like Manifold does when installing it, and verify every
 * image against its manifest.
 *
 * Fails the build if Manifold would refuse the bundle.
 */
void verify_bundle(const fs::path &bundle_path);

/**
 * Sign an existing bundle in place, replacing any signature it has.
 *
 * Fails if Manifold would refuse the bundle, e.g. because a listed file does
 * not match index.json or an indexed file is missing.
 */
void sign_bundle(const fs::path &bundle_path, const fs::path &sign_key);

} // namespace mfb

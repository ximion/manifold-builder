/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "manifest.hpp"
#include "utils.hpp"

namespace mfb
{

/**
 * A checksum algorithm.
 */
enum class ChecksumKind {
    Blake3,
    Sha256,
};

/**
 * Compute the checksum of a string as lowercase hex.
 */
std::string checksum_data_hex(std::string_view data, ChecksumKind kind);

/**
 * Compute the checksum of a file's contents as lowercase hex.
 */
std::string checksum_file_hex(const fs::path &path, ChecksumKind kind);

/**
 * Compute the blake3 hash of a directory tree as lowercase hex.
 * Every copy of the tree hashes the same, regardless of timestamps or the
 * order in which the filesystem lists its entries.
 */
std::string tree_hash(const fs::path &dir);

/**
 * Compute the identity hash of an image.
 * The identity hash uniquely identifies the makeup of an image, but is version independent,
 * so any rebuilds or version upgrades will not change the identity of an image/component.
 */
std::string identity_hash(std::string_view base, std::string_view architecture, const InstalledPackages &packages);

/**
 * Compute the content hash of an image.
 * The content hash is unique to one, and one version only. If it differs, the contents of an image
 * are also different. It is identical for images that contain the same stuff.
 */
std::string content_hash(
    std::string_view base,
    std::string_view architecture,
    std::string_view component,
    std::string_view version,
    const InstalledPackages &packages,
    const std::vector<std::string> &extra_lines = {});

} // namespace mfb

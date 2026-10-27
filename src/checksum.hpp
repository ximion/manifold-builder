/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <string>
#include <string_view>

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
 * Both checksums of a file, as lowercase hex.
 */
struct FileChecksums {
    std::string blake3;
    std::string sha256;
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
 * Compute both checksums of a file's contents, reading it once.
 */
FileChecksums checksum_file(const fs::path &path);

} // namespace mfb

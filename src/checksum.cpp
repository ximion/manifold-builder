/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "checksum.hpp"

#include "error.hpp"

#include <blake3.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <glib.h>
#include <utility>

namespace mfb
{

namespace
{

std::string hex(const uint8_t *bytes, size_t length)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0xf];
    }
    return out;
}

std::string finish(blake3_hasher &hasher)
{
    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, digest, sizeof digest);
    return hex(digest, sizeof digest);
}

/**
 * Read a file in chunks and pass each one to `update`.
 */
void read_chunks(const fs::path &path, const std::function<void(const char *, size_t)> &update)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        fail("Cannot read {}: {}", path.string(), std::strerror(errno));
    std::vector<char> buffer(1 << 20);
    while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0)
        update(buffer.data(), static_cast<size_t>(file.gcount()));
    // a read error, e.g. of a directory, sets badbit, the end of the file does not
    if (file.bad())
        fail("Cannot read {}: {}", path.string(), std::strerror(errno));
}

} // namespace

std::string checksum_data_hex(std::string_view data, ChecksumKind kind)
{
    switch (kind) {
    case ChecksumKind::Blake3: {
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_update(&hasher, data.data(), data.size());
        return finish(hasher);
    }
    case ChecksumKind::Sha256: {
        g_autofree gchar *digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(data.data()), data.size());
        return digest;
    }
    }
    std::unreachable();
}

std::string checksum_file_hex(const fs::path &path, ChecksumKind kind)
{
    switch (kind) {
    case ChecksumKind::Blake3: {
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        read_chunks(path, [&](const char *data, size_t size) {
            blake3_hasher_update(&hasher, data, size);
        });
        return finish(hasher);
    }
    case ChecksumKind::Sha256: {
        g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
        read_chunks(path, [&](const char *data, size_t size) {
            g_checksum_update(checksum, reinterpret_cast<const guchar *>(data), static_cast<gssize>(size));
        });
        return g_checksum_get_string(checksum);
    }
    }
    std::unreachable();
}

FileChecksums checksum_file(const fs::path &path)
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    read_chunks(path, [&](const char *data, size_t size) {
        blake3_hasher_update(&hasher, data, size);
        g_checksum_update(checksum, reinterpret_cast<const guchar *>(data), static_cast<gssize>(size));
    });
    return {.blake3 = finish(hasher), .sha256 = g_checksum_get_string(checksum)};
}

} // namespace mfb

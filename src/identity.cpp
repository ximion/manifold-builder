/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "identity.hpp"

#include "error.hpp"
#include "utils.hpp"

#include <algorithm>
#include <blake3.h>
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
    std::ifstream file(path, std::ios::binary);
    if (!file)
        fail("Cannot read {}", path.string());

    std::vector<char> buffer(1 << 20);
    auto read_chunk = [&] {
        return file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0;
    };
    switch (kind) {
    case ChecksumKind::Blake3: {
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        while (read_chunk())
            blake3_hasher_update(&hasher, buffer.data(), static_cast<size_t>(file.gcount()));
        return finish(hasher);
    }
    case ChecksumKind::Sha256: {
        g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
        while (read_chunk())
            g_checksum_update(checksum, reinterpret_cast<const guchar *>(buffer.data()), file.gcount());
        return g_checksum_get_string(checksum);
    }
    }
    std::unreachable();
}

std::string tree_hash(const fs::path &dir)
{
    // We hash a directory tree over a line naming the format, followed by one
    // line per entry, sorted by path relative to the tree:
    //
    //   manifold-tree/1
    //   d <path>                     a directory
    //   f <path> <x|-> <blake3>      a file, its executable bit and its contents
    //   l <path> <target>            a symbolic link
    //
    // Ownership and timestamps are left out, so every copy of a tree hashes the same.

    std::vector<std::pair<std::string, fs::directory_entry>> entries;
    for (const auto &entry : fs::recursive_directory_iterator(dir))
        entries.emplace_back(entry.path().lexically_relative(dir).generic_string(), entry);
    std::ranges::sort(entries, {}, [](const auto &entry) -> const std::string & {
        return entry.first;
    });

    std::string text = "manifold-tree/1\n";
    for (const auto &[path, entry] : entries) {
        if (entry.is_symlink()) {
            text += std::format("l {} {}\n", path, fs::read_symlink(entry.path()).string());
        } else if (entry.is_directory()) {
            text += std::format("d {}\n", path);
        } else if (entry.is_regular_file()) {
            bool executable = (entry.status().permissions() & fs::perms::owner_exec) != fs::perms::none;
            text += std::format(
                "f {} {} {}\n", path, executable ? "x" : "-", checksum_file_hex(entry.path(), ChecksumKind::Blake3));
        }
    }
    return checksum_data_hex(text, ChecksumKind::Blake3);
}

std::string identity_hash(std::string_view base, std::string_view architecture, const InstalledPackages &packages)
{
    // The hash inputs are line-based text. The first line names the format, so
    // a later revision can never collide with this one.
    //
    // An application's identity is its content hash, because it is one exact
    // build that nothing else can stand in for.

    std::string text = std::format("manifold-identity/1\nbase: {}\narchitecture: {}\n", base, architecture);
    for (const auto &[name, version] : packages)
        text += name + "\n";
    return checksum_data_hex(text, ChecksumKind::Blake3);
}

std::string content_hash(
    std::string_view base,
    std::string_view architecture,
    std::string_view component,
    std::string_view version,
    const InstalledPackages &packages,
    const std::vector<std::string> &extra_lines)
{
    std::string text = std::format(
        "manifold-content/1\nbase: {}\narchitecture: {}\ncomponent: {}\nversion: {}\n",
        base,
        architecture,
        component,
        version);
    for (const auto &line : extra_lines)
        text += line + "\n";
    for (const auto &[name, version] : packages)
        text += name + " " + version + "\n";
    return checksum_data_hex(text, ChecksumKind::Blake3);
}

} // namespace mfb

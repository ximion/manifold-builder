/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "identity.hpp"

#include "checksum.hpp"
#include "utils.hpp"

#include <algorithm>

namespace mfb
{

std::string tree_hash(const fs::path &dir, const std::function<bool(const fs::path &)> &skip)
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

    std::vector<std::string> paths;
    for (const auto &path : list_tree(dir, skip))
        paths.push_back(path.generic_string());
    std::ranges::sort(paths);

    std::string text = "manifold-tree/1\n";
    for (const auto &path : paths) {
        auto entry = dir / path;
        auto status = fs::symlink_status(entry);
        if (fs::is_symlink(status)) {
            text += std::format("l {} {}\n", path, fs::read_symlink(entry).string());
        } else if (fs::is_directory(status)) {
            text += std::format("d {}\n", path);
        } else if (fs::is_regular_file(status)) {
            bool executable = (status.permissions() & fs::perms::owner_exec) != fs::perms::none;
            text += std::format(
                "f {} {} {}\n", path, executable ? "x" : "-", checksum_file_hex(entry, ChecksumKind::Blake3));
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

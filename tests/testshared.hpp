/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "checksum.hpp"
#include "error.hpp"
#include "manifest.hpp"
#include "recipe.hpp"
#include "utils.hpp"

#include <string>
#include <string_view>
#include <unistd.h>

namespace mfb::test
{

/**
 * Get the tests/ directory of the source tree.
 */
inline fs::path tests_dir()
{
    return MFB_TESTS_DIR;
}

/**
 * Temporary directory that is removed with its contents on destruction.
 */
class TempDir
{
public:
    explicit TempDir(std::string_view name)
        : m_path(fs::temp_directory_path() / std::format("mfb-test-{}-{}", name, getpid()))
    {
        fs::remove_all(m_path);
        fs::create_directories(m_path);
    }

    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(m_path, ignored);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    const fs::path &path() const
    {
        return m_path;
    }

    fs::path operator/(const fs::path &relative) const
    {
        return m_path / relative;
    }

private:
    fs::path m_path;
};

/**
 * Write a file that stands in for an image to `dir`, and describe it with
 * made-up hashes that still look valid, so Manifold accepts its manifest.
 */
inline ImageFile fake_image(const fs::path &dir, const std::string &file)
{
    write_file(dir / file, "not really an image");
    return ImageFile{
        .file = file,
        .size = fs::file_size(dir / file),
        .blake3 = checksum_file_hex(dir / file, ChecksumKind::Blake3),
        .sha256 = checksum_file_hex(dir / file, ChecksumKind::Sha256),
        // a store keeps its images by verity hash, so each one needs its own
        .verity_root_hash = checksum_data_hex("verity " + file, ChecksumKind::Sha256),
        .data_offset = 1,
        .hash_offset = 2,
    };
}

/**
 * Check whether parsing the recipe text fails.
 */
inline bool recipe_rejected(std::string_view yaml, const fs::path &dir = fs::current_path())
{
    try {
        parse_recipe(yaml, "inline", dir);
    } catch (const BuildError &) {
        return true;
    }
    return false;
}

/**
 * Start of a valid app recipe that tests append keys to.
 */
constexpr std::string_view APP_HEADER = "component: x.y\nversion: 1\nbase: c.d\nbase-version: 1\ncommand: x\n";

/**
 * Start of a core recipe, which still needs CORE_BOOTSTRAP to be valid.
 */
constexpr std::string_view CORE_HEADER = "type: core\ncomponent: x.y\nversion: 1\nname: x\nsummary: x\n";

/**
 * A bootstrap section for CORE_HEADER.
 */
constexpr std::string_view CORE_BOOTSTRAP = "bootstrap: {suite: trixie, snapshot: '2026-10-04T00:00:00Z'}\n";

/**
 * Start of a valid platform recipe that tests append keys to.
 */
constexpr std::string_view PLATFORM_HEADER =
    "type: platform\ncomponent: x.y\nversion: 1\nname: x\nsummary: x\nbase: c.d\n";

} // namespace mfb::test

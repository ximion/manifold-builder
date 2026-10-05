/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "error.hpp"
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

} // namespace mfb::test

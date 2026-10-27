/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "build.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>

using namespace mfb;
using mfb::test::TempDir;

namespace
{

/**
 * Make a file or directory look as if nobody used it for 40 days.
 */
void make_old(const fs::path &path)
{
    timespec old = {.tv_sec = time(nullptr) - 40 * 24 * 3600, .tv_nsec = 0};
    const timespec times[2] = {old, old};
    REQUIRE(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
}

} // namespace

TEST_CASE("the cache keeps what builds used lately", "[build]")
{
    TempDir dir("cache");
    const auto &cache = dir.path();
    for (const auto *file :
         {"downloads/old",
          "downloads/new",
          "packages/old.deb",
          "packages/new.deb",
          "packages/lock",
          "packages/partial/old.deb"}) {
        fs::create_directories((cache / file).parent_path());
        write_file(cache / file, "");
    }
    for (const auto *mirror : {"git/old.git", "git/new.git", "git/busy.git"})
        fs::create_directories(cache / mirror / "objects");
    for (const auto *path :
         {"downloads/old",
          "packages/old.deb",
          "packages/lock",
          "packages/partial/old.deb",
          "git/old.git",
          "git/busy.git"})
        make_old(cache / path);

    // another build is using this mirror
    FileLock busy(cache / "git/busy.git.lock");

    CHECK(prune_cache(cache, std::chrono::days(30)) == 4);
    CHECK(!fs::exists(cache / "downloads/old"));
    CHECK(!fs::exists(cache / "packages/old.deb"));
    CHECK(!fs::exists(cache / "packages/partial/old.deb"));
    CHECK(!fs::exists(cache / "git/old.git"));
    CHECK(fs::exists(cache / "downloads/new"));
    CHECK(fs::exists(cache / "packages/new.deb"));
    CHECK(fs::exists(cache / "git/new.git"));
    CHECK(fs::exists(cache / "git/busy.git"));
    // apt's lock is no package
    CHECK(fs::exists(cache / "packages/lock"));
    // a cache without some of its directories is fine
    CHECK(prune_cache(dir / "missing", std::chrono::days(30)) == 0);
}

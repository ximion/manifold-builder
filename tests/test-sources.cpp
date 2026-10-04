/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "identity.hpp"
#include "process.hpp"
#include "sources/source.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <zip.h>

using namespace mfb;
using mfb::test::TempDir;

namespace
{

SourceRecord fetch(const Source &source, const fs::path &destination, const TempDir &dir)
{
    REQUIRE(check_source(source).empty());
    return source_fetcher_for(source.type)->fetch(source, destination, {.dir = dir.path(), .scratch = dir / "scratch"});
}

/**
 * Create a small project with a README and src/main.c.
 */
void make_project(const fs::path &dir)
{
    fs::create_directories(dir / "src");
    write_file(dir / "README", "hello\n");
    write_file(dir / "src/main.c", "int main(void) { return 0; }\n");
}

} // namespace

TEST_CASE("dir sources", "[sources]")
{
    TempDir dir("source-dir");
    make_project(dir / "project");
    auto record = fetch({.type = "dir", .path = "project"}, dir / "out", dir);
    CHECK(read_file(dir / "out/src/main.c") == "int main(void) { return 0; }\n");
    CHECK(record.type == "dir");
    CHECK(record.path == "project");
    CHECK(record.pin() == tree_hash(dir / "project"));
    CHECK_THROWS_AS(fetch({.type = "dir", .path = "missing"}, dir / "out", dir), BuildError);
}

TEST_CASE("file sources", "[sources]")
{
    TempDir dir("source-file");
    write_file(dir / "launcher.sh", "#!/bin/sh\n");
    auto sha256 = checksum_file_hex(dir / "launcher.sh", ChecksumKind::Sha256);

    auto record = fetch({.type = "file", .path = "launcher.sh"}, dir / "out", dir);
    CHECK(fs::exists(dir / "out/launcher.sh"));
    CHECK(record.sha256 == sha256);
    fetch({.type = "file", .path = "launcher.sh", .sha256 = sha256, .dest_filename = "run"}, dir / "out", dir);
    CHECK(fs::exists(dir / "out/run"));

    Source wrong{.type = "file", .path = "launcher.sh", .sha256 = checksum_data_hex("other", ChecksumKind::Sha256)};
    CHECK_THROWS_AS(fetch(wrong, dir / "out", dir), BuildError);
}

TEST_CASE("patch sources apply to the sources before them", "[sources]")
{
    TempDir dir("source-patch");
    make_project(dir / "project");
    write_file(
        dir / "fix.patch",
        "--- a/README\n"
        "+++ b/README\n"
        "@@ -1 +1 @@\n"
        "-hello\n"
        "+hello, patched\n");
    fetch({.type = "dir", .path = "project"}, dir / "out", dir);
    auto record = fetch({.type = "patch", .path = "fix.patch"}, dir / "out", dir);
    CHECK(read_file(dir / "out/README") == "hello, patched\n");
    CHECK(record.sha256 == checksum_file_hex(dir / "fix.patch", ChecksumKind::Sha256));
    // applying it again fails instead of reversing the patch
    CHECK_THROWS_AS(fetch({.type = "patch", .path = "fix.patch"}, dir / "out", dir), BuildError);
    CHECK(read_file(dir / "out/README") == "hello, patched\n");
}

TEST_CASE("tar archives", "[sources]")
{
    TempDir dir("source-tar");
    make_project(dir / "project-1.0");
    run({.argv = {"tar", "-czf", "project-1.0.tar.gz", "project-1.0"}, .cwd = dir.path()});
    auto sha256 = checksum_file_hex(dir / "project-1.0.tar.gz", ChecksumKind::Sha256);

    auto record = fetch({.type = "archive", .path = "project-1.0.tar.gz", .sha256 = sha256}, dir / "out", dir);
    CHECK(fs::exists(dir / "out/src/main.c"));
    CHECK(record.sha256 == sha256);
    fetch({.type = "archive", .path = "project-1.0.tar.gz", .strip_components = 0}, dir / "kept", dir);
    CHECK(fs::exists(dir / "kept/project-1.0/README"));
}

TEST_CASE("zip archives", "[sources]")
{
    if (!find_tool("unzip"))
        SKIP("unzip is not installed");
    TempDir dir("source-zip");
    zip_t *archive = zip_open((dir / "project.zip").c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    REQUIRE(archive != nullptr);
    std::string readme = "hello\n";
    zip_dir_add(archive, "project-1.0/", ZIP_FL_ENC_UTF_8);
    zip_dir_add(archive, "project-1.0/src/", ZIP_FL_ENC_UTF_8);
    zip_file_add(
        archive,
        "project-1.0/src/README",
        zip_source_buffer(archive, readme.data(), readme.size(), 0),
        ZIP_FL_ENC_UTF_8);
    REQUIRE(zip_close(archive) == 0);

    fetch({.type = "archive", .path = "project.zip"}, dir / "out", dir);
    CHECK(read_file(dir / "out/src/README") == "hello\n");
    CHECK(!fs::exists(dir / "out/project-1.0"));
}

TEST_CASE("git sources", "[sources]")
{
    if (!find_tool("git"))
        SKIP("git is not installed");
    TempDir dir("source-git");
    make_project(dir / "repo");
    auto git = [&](std::vector<std::string> arguments) {
        arguments.insert(
            arguments.begin(),
            {"git", "-C", (dir / "repo").string(), "-c", "user.name=Test", "-c", "user.email=test@example.org"});
        return run_capture({.argv = arguments, .quiet = true});
    };
    git({"init", "--quiet", "--initial-branch=main"});
    git({"add", "."});
    git({"commit", "--quiet", "-m", "initial"});
    git({"tag", "v1.0"});
    auto commit = git({"rev-parse", "HEAD"});

    auto record = fetch({.type = "git", .url = (dir / "repo").string(), .tag = "v1.0"}, dir / "out", dir);
    CHECK(record.pin() == commit);
    CHECK(fs::exists(dir / "out/src/main.c"));

    // after an earlier source, the checkout is copied over its files
    write_file(dir / "extra", "x");
    fetch({.type = "file", .path = "extra"}, dir / "both", dir);
    fetch({.type = "git", .url = (dir / "repo").string()}, dir / "both", dir);
    CHECK(fs::exists(dir / "both/extra"));
    CHECK(fs::exists(dir / "both/README"));

    Source mismatch{
        .type = "git",
        .url = (dir / "repo").string(),
        .tag = "v1.0",
        .commit = checksum_data_hex("x", ChecksumKind::Sha256).substr(0, 40)};
    CHECK_THROWS_AS(fetch(mismatch, dir / "mismatch", dir), BuildError);
}

TEST_CASE("every part gets a source directory, sources go to their dest", "[sources]")
{
    TempDir dir("fetch-sources");
    make_project(dir / "project");
    write_file(dir / "data.txt", "data\n");
    Part part{
        .name = "p",
        .buildsystem = "meson",
        .sources = {{.type = "dir", .path = "project"}, {.type = "file", .dest = "extra/data", .path = "data.txt"}},
        .dir = dir.path(),
    };
    Part empty{.name = "nothing", .buildsystem = "simple", .build_commands = {"true"}};
    auto records = fetch_sources({&part, &empty}, dir / "sources", dir / "scratch");
    REQUIRE(records.size() == 2);
    CHECK(records[0].part == "p");
    CHECK(fs::exists(dir / "sources/p/src/main.c"));
    CHECK(fs::exists(dir / "sources/p/extra/data/data.txt"));
    CHECK(fs::is_directory(dir / "sources/nothing"));
}

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "checksum.hpp"
#include "identity.hpp"
#include "process.hpp"
#include "sources/source.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <zip.h>

using namespace mfb;
using Catch::Matchers::ContainsSubstring;
using mfb::test::TempDir;

namespace
{

SourceRecord fetch(const Source &source, const fs::path &destination, const TempDir &dir)
{
    REQUIRE(check_source(source).empty());
    return source_fetcher_for(source.type)
        ->fetch(source, destination, {.dir = dir.path(), .scratch = dir / "scratch", .cache = dir / "cache"});
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

    // modes and times are kept
    fs::permissions(dir / "project/src", fs::perms::owner_write, fs::perm_options::remove);
    auto copied = fetch({.type = "dir", .path = "project"}, dir / "copied", dir);
    CHECK((fs::status(dir / "copied/src").permissions() & fs::perms::owner_write) == fs::perms::none);
    CHECK(fs::last_write_time(dir / "copied/src/main.c") == fs::last_write_time(dir / "project/src/main.c"));
    CHECK(copied.pin() == record.pin());
    fs::permissions(dir / "project/src", fs::perms::owner_write, fs::perm_options::add);
    fs::permissions(dir / "copied/src", fs::perms::owner_write, fs::perm_options::add);

    // hidden files are left out, so git does not change the hash
    fs::create_directories(dir / "project/.git");
    write_file(dir / "project/.git/index", "changes all the time\n");
    write_file(dir / "project/src/.main.c.swp", "x");
    auto hidden = fetch({.type = "dir", .path = "project"}, dir / "hidden", dir);
    CHECK(fs::exists(dir / "hidden/src/main.c"));
    CHECK(!fs::exists(dir / "hidden/.git"));
    CHECK(!fs::exists(dir / "hidden/src/.main.c.swp"));
    CHECK(hidden.pin() == record.pin());
}

TEST_CASE("a dir source leaves out work dirs and the output", "[sources]")
{
    // the recipe next to the code, with the work dir and the bundle beside it
    TempDir dir("source-dir-own");
    make_project(dir.path());
    auto work = dir / "_build-app";
    fs::create_directories(work / "rootfs");
    write_file(dir / "app.manifold", "an earlier bundle");
    // the work dir of another recipe, kept with --keep-work
    fs::create_directories(dir / "_build-other/rootfs");
    write_file(dir / "_build-other/.gitignore", WORK_DIR_GITIGNORE_DATA);
    // any other directory with a .gitignore is a source
    fs::create_directories(dir / "docs");
    write_file(dir / "docs/.gitignore", "*.html\n");
    write_file(dir / "docs/index.md", "# hello\n");
    auto clean = tree_hash(dir.path(), [&](const fs::path &path) {
        return path.filename().string().starts_with('.') || path == work || path == dir / "app.manifold"
               || path == dir / "_build-other";
    });

    auto record = source_fetcher_for("dir")->fetch(
        {.type = "dir", .path = "."},
        work / "sources/app",
        {.dir = dir.path(), .scratch = work / "fetch", .cache = work / "cache", .skip = {work, dir / "app.manifold"}});
    CHECK(read_file(work / "sources/app/README") == "hello\n");
    CHECK(fs::exists(work / "sources/app/docs/index.md"));
    CHECK(!fs::exists(work / "sources/app/_build-app"));
    CHECK(!fs::exists(work / "sources/app/_build-other"));
    CHECK(!fs::exists(work / "sources/app/app.manifold"));
    CHECK(record.pin() == clean);
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

TEST_CASE("downloads are kept in the cache", "[sources]")
{
    if (!find_tool("curl"))
        SKIP("curl is not installed");
    TempDir dir("source-download");
    write_file(dir / "served.sh", "#!/bin/sh\n");
    auto sha256 = checksum_file_hex(dir / "served.sh", ChecksumKind::Sha256);
    Source source{.type = "file", .url = "file://" + (dir / "served.sh").string(), .sha256 = sha256};

    fetch(source, dir / "out", dir);
    CHECK(read_file(dir / "cache/downloads" / sha256) == "#!/bin/sh\n");
    // a later build needs no network
    fs::remove(dir / "served.sh");
    fetch(source, dir / "again", dir);
    CHECK(read_file(dir / "again/served.sh") == "#!/bin/sh\n");

    // a download that fails its checksum is not kept
    write_file(dir / "served.sh", "changed\n");
    Source changed{.type = "file", .url = source.url, .sha256 = checksum_data_hex("x", ChecksumKind::Sha256)};
    CHECK_THROWS_AS(fetch(changed, dir / "changed", dir), BuildError);
    CHECK(std::distance(fs::directory_iterator(dir / "cache/downloads"), {}) == 1);
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

TEST_CASE("file names of sources", "[sources]")
{
    CHECK(source_file_name({.type = "file", .path = "data/launcher.sh"}) == "launcher.sh");
    // a local file may have any name
    CHECK(source_file_name({.type = "file", .path = "a#b?c"}) == "a#b?c");
    CHECK(source_file_name({.type = "file", .url = "https://example.org/v1.zip?download=1#top"}) == "v1.zip");

    // a download link with a query is still unpacked as a zip
    Source zip{.type = "archive", .url = "https://example.org/v1.zip?download=1", .sha256 = "x"};
    CHECK(source_fetcher_for("archive")->tools(zip) == std::vector<std::string>{"unzip", "curl"});
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

    // a commit the mirror has needs no origin
    fs::rename(dir / "repo", dir / "moved");
    auto pinned = fetch({.type = "git", .url = (dir / "repo").string(), .commit = commit}, dir / "pinned", dir);
    CHECK(pinned.pin() == commit);
    // a newer commit is fetched into the mirror
    fs::rename(dir / "moved", dir / "repo");
    write_file(dir / "repo/NEWS", "news\n");
    git({"add", "NEWS"});
    git({"commit", "--quiet", "-m", "news"});
    auto latest = fetch({.type = "git", .url = (dir / "repo").string(), .branch = "main"}, dir / "latest", dir);
    CHECK(latest.pin() == git({"rev-parse", "HEAD"}));
    CHECK(fs::exists(dir / "latest/NEWS"));
    // the checkout's origin is the source, not the mirror
    CHECK(
        run_capture({.argv = {"git", "-C", (dir / "latest").string(), "remote", "get-url", "origin"}})
        == (dir / "repo").string());

    // a tag must point at the commit given with it
    Source mismatch{.type = "git", .url = (dir / "repo").string(), .tag = "v1.0", .commit = latest.pin()};
    CHECK_THROWS_WITH(
        fetch(mismatch, dir / "mismatch", dir),
        ContainsSubstring(std::format("Tag v1.0 of {} is commit {}, not {}", mismatch.url, commit, latest.pin())));
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
    auto records = fetch_sources(
        {&part, &empty}, dir / "sources", {.scratch = dir / "scratch", .cache = dir / "cache"});
    REQUIRE(records.size() == 2);
    CHECK(records[0].part == "p");
    CHECK(fs::exists(dir / "sources/p/src/main.c"));
    CHECK(fs::exists(dir / "sources/p/extra/data/data.txt"));
    CHECK(fs::is_directory(dir / "sources/nothing"));
}

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "process.hpp"
#include "sandbox.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sys/xattr.h>
#include <unistd.h>

using namespace mfb;
using mfb::test::TempDir;

namespace
{

/**
 * Run a script like Sandbox::run does, minus the namespace, and describe
 * its failure.
 */
std::string failure_of(const Script &script)
{
    TempDir dir("sandbox");
    auto report = dir / "report";
    auto text = std::format("exec {}>{}{}", REPORT_FD, shell_quote(report.string()), script.text());
    int status = run_status({.argv = {"bash", "-c", text}, .quiet = true});
    REQUIRE(status != 0);
    return describe_failure(script, read_file(report), status);
}

void run_traced(Script &s, std::string_view commands)
{
    s.run({"bash", "-Eeuo", "pipefail", "-c", traced_script(commands)});
}

} // namespace

TEST_CASE("traced scripts show the recipe's commands only", "[sandbox]")
{
    auto output = run_capture({.argv = {"bash", "-c", "exec 2>&1\n" + traced_script("echo hello\n")}});
    CHECK(output == "+ echo hello\nhello");
}

TEST_CASE("a failing recipe command is reported with its step", "[sandbox]")
{
    Script s;
    s.run({"true"});
    s.begin_step("action 'greet'");
    run_traced(s, "echo hello\nls /nonexistent\necho never\n");
    s.end_step();
    CHECK(failure_of(s) == "action 'greet': Command failed with status 2: ls /nonexistent");
}

TEST_CASE("the recipe's own line is reported", "[sandbox]")
{
    Script s;
    s.begin_step("part 'x'");
    run_traced(s, "greeting=$(false)\n");
    s.end_step();
    CHECK(failure_of(s) == "part 'x': Command failed with status 1: greeting=$(false)");

    // a failure that did not stop the script is not reported
    Script t;
    t.begin_step("part 'x'");
    run_traced(t, "echo \"$(false; echo hello)\"\nls /nonexistent\n");
    t.end_step();
    CHECK(failure_of(t) == "part 'x': Command failed with status 2: ls /nonexistent");
}

TEST_CASE("a step fails in a command of our own", "[sandbox]")
{
    Script s;
    s.begin_step("bootstrapping the core");
    s.run({"true"});
    s.end_step();
    s.begin_step("installing packages");
    s.run({"bash", "-c", "exit 100"});
    s.end_step();
    CHECK(failure_of(s) == "installing packages failed with status 100");
}

TEST_CASE("overlay markers are removed from the marked directories", "[sandbox]")
{
    if (!find_tool("getfattr") || !find_tool("setfattr"))
        SKIP("getfattr or setfattr is not installed");
    TempDir dir("markers");
    auto mark = [](const fs::path &path, const char *name) {
        return setxattr(path.c_str(), name, "y", 1, 0) == 0;
    };
    auto marked = [](const fs::path &path, const char *name) {
        return getxattr(path.c_str(), name, nullptr, 0) >= 0;
    };
    fs::create_directories(dir / "usr/lib");
    fs::create_directories(dir / "with space");
    // an upper layer is full of links into the layers below it
    fs::create_symlink("libfoo.so.1", dir / "usr/lib/libfoo.so");
    if (!mark(dir / "usr/lib", "user.overlay.opaque"))
        SKIP("the temporary directory has no user extended attributes");
    REQUIRE(mark(dir / "with space", "user.overlay.opaque"));
    REQUIRE(mark(dir / "usr", "user.other"));

    Script s;
    strip_overlay_markers(s, dir.path());
    CHECK(run_status({.argv = {"bash", "-c", s.text()}}) == 0);
    CHECK(!marked(dir / "usr/lib", "user.overlay.opaque"));
    CHECK(!marked(dir / "with space", "user.overlay.opaque"));
    CHECK(marked(dir / "usr", "user.other"));

    // a marker that stays fails the script
    if (geteuid() != 0) {
        fs::create_directories(dir / "locked");
        REQUIRE(mark(dir / "locked", "user.overlay.opaque"));
        fs::permissions(dir / "locked", fs::perms::owner_write, fs::perm_options::remove);
        CHECK(run_status({.argv = {"bash", "-c", s.text()}, .quiet = true}) != 0);
        fs::permissions(dir / "locked", fs::perms::owner_write, fs::perm_options::add);
    }
}

TEST_CASE("a failure outside of any step", "[sandbox]")
{
    Script s;
    s.begin_step("part 'x'");
    s.run({"true"});
    s.end_step();
    s.run({"false"});
    CHECK(failure_of(s) == "Build script failed with status 1 (run with --verbose to see the script)");
}

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "buildsystems/buildsystem.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace mfb;

namespace
{

const InstallTarget APP = {"/app", "lib"};
const InstallTarget CORE = {"/usr", "lib/x86_64-linux-gnu"};

bool contains(const std::string &text, std::string_view what)
{
    return text.find(what) != std::string::npos;
}

std::string script_for(
    const Part &part,
    const InstallTarget &target,
    BuildSettings settings = {},
    const fs::path &source_dir = "/nonexistent")
{
    return part_build_script(part, target, settings, source_dir);
}

void write_executable(const fs::path &path)
{
    write_file(path, "#!/bin/sh\n");
    fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
}

} // namespace

TEST_CASE("meson", "[buildsystems]")
{
    Part part{.name = "x", .buildsystem = "meson", .config_opts = {"-Dfoo=bar"}, .make_args = {"-v"}};
    auto script = script_for(part, APP);
    CHECK(contains(script, "meson setup _mfold-build --prefix=/app --libdir=lib --buildtype=release -Dfoo=bar"));
    CHECK(contains(script, "ninja -C _mfold-build -j\"$MANIFOLD_BUILDER_N_JOBS\" -v\n"));
    CHECK(contains(script, "ninja -C _mfold-build install\n"));
    // a core part keeps its libraries in the multiarch directory
    CHECK(contains(script_for(part, CORE), "--prefix=/usr --libdir=lib/x86_64-linux-gnu"));
}

TEST_CASE("cmake", "[buildsystems]")
{
    Part part{.name = "x", .buildsystem = "cmake", .config_opts = {"-DFOO=ON"}, .make_install_args = {"-v"}};
    auto script = script_for(part, APP);
    CHECK(
        contains(script, "cmake -S . -B _mfold-build -G Ninja -DCMAKE_INSTALL_PREFIX=/app -DCMAKE_INSTALL_LIBDIR=lib"));
    CHECK(contains(script, "-DCMAKE_BUILD_TYPE=Release -DFOO=ON\n"));
    CHECK(contains(script, "ninja -C _mfold-build -j\"$MANIFOLD_BUILDER_N_JOBS\"\n"));
    CHECK(contains(script, "ninja -C _mfold-build install -v\n"));

    part.buildsystem = "cmake-make";
    auto make = script_for(part, APP);
    CHECK(contains(make, "-G 'Unix Makefiles'"));
    CHECK(contains(make, "make -C _mfold-build -j\"$MANIFOLD_BUILDER_N_JOBS\"\n"));
    CHECK(contains(make, "make -C _mfold-build install -v\n"));
}

TEST_CASE("autotools", "[buildsystems]")
{
    Part part{.name = "x", .buildsystem = "autotools", .config_opts = {"--disable-foo"}};
    auto script = script_for(part, APP);
    // without a configure script or one that generates it, autoreconf does
    CHECK(script.starts_with("autoreconf -fi\n./configure --prefix=/app --libdir=/app/lib --disable-foo\n"));
    CHECK(contains(script, "make -j\"$MANIFOLD_BUILDER_N_JOBS\"\n"));
    CHECK(contains(script, "make install\n"));
    // with a /usr prefix, configuration goes to /etc
    CHECK(contains(
        script_for(part, CORE),
        "./configure --prefix=/usr --libdir=/usr/lib/x86_64-linux-gnu --sysconfdir=/etc --localstatedir=/var"));
}

TEST_CASE("autotools prefers the project's own configure script", "[buildsystems]")
{
    test::TempDir sources("autotools");
    Part part{.name = "x", .buildsystem = "autotools"};
    write_executable(sources / "bootstrap");
    write_executable(sources / "autogen.sh");
    CHECK(script_for(part, APP, {}, sources.path()).starts_with("NOCONFIGURE=1 ./autogen.sh\n./configure "));
    write_executable(sources / "configure");
    CHECK(script_for(part, APP, {}, sources.path()).starts_with("./configure "));
}

TEST_CASE("simple", "[buildsystems]")
{
    Part part{.name = "x", .buildsystem = "simple", .build_commands = {"make", "make install PREFIX=$MANIFOLD_DEST"}};
    CHECK(script_for(part, APP) == "make\nmake install PREFIX=$MANIFOLD_DEST\n");
}

TEST_CASE("the parts of a part's script", "[buildsystems]")
{
    Part part{
        .name = "x",
        .buildsystem = "meson",
        .subdir = "sub dir",
        .config_opts = {"-Dpart=true"},
        .build_commands = {"echo between"},
        .post_install = {"echo after"},
    };
    BuildSettings settings{.config_opts = {"-Dglobal=true"}};
    auto script = script_for(part, APP, settings);
    // the caller runs the script in the subdir
    CHECK(!contains(script, "sub dir"));
    // build-options come first, so that the part's own config-opts win
    CHECK(contains(script, "-Dglobal=true -Dpart=true"));
    // build-commands run between building and installing, as in flatpak-builder
    CHECK(script.find("ninja -C _mfold-build -j") < script.find("echo between"));
    CHECK(script.find("echo between") < script.find("ninja -C _mfold-build install"));
    // the caller runs post-install
    CHECK(!contains(script, "echo after"));
}

TEST_CASE("unknown build systems", "[buildsystems]")
{
    CHECK(build_system_known("cmake-make"));
    CHECK(!build_system_known("qmake"));
    CHECK_THROWS_AS(build_system_for("qmake"), BuildError);
}

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "recipe.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace mfb;
using mfb::test::APP_HEADER;
using mfb::test::CORE_BOOTSTRAP;
using mfb::test::CORE_HEADER;
using mfb::test::PLATFORM_HEADER;
using mfb::test::recipe_rejected;
using mfb::test::TempDir;

namespace
{

std::vector<std::string> names(const std::vector<const Part *> &parts)
{
    std::vector<std::string> result;
    for (const auto *part : parts)
        result.push_back(part->name);
    return result;
}

} // namespace

TEST_CASE("the sample recipes load", "[recipe]")
{
    auto samples = test::tests_dir() / "samples";
    auto core = load_recipe(samples / "net.debian.core13.yaml");
    CHECK(core.is_core());
    CHECK(core.component == "net.debian.core13");
    REQUIRE(core.bootstrap);
    CHECK(core.bootstrap->suite == "trixie");
    CHECK(!core.sdk_only.empty());
    CHECK(core.packages.at("apt").size() > 10);
    REQUIRE(core.parts.size() == 1);
    CHECK(std::get<Part>(core.parts[0]).post_install.size() == 3);

    auto test_app = load_recipe(samples / "com.example.mftestapp.yaml");
    CHECK(test_app.command == "mftestapp");
    CHECK(!test_app.generate_sdk);

    auto platform = load_recipe(samples / "net.debian.core13.kf6.yaml");
    CHECK(platform.is_platform());
}

TEST_CASE("the recipe hash ignores formatting, not content", "[recipe]")
{
    auto path = test::tests_dir() / "samples" / "com.example.mftestapp.yaml";
    auto recipe = load_recipe(path);
    auto reformatted = parse_recipe("# a comment\n" + read_file(path) + "\n\n", "inline", path.parent_path());
    CHECK(!recipe.hash.empty());
    CHECK(reformatted.hash == recipe.hash);

    auto other = parse_recipe(
        std::string(APP_HEADER) + "parts: [{name: p, buildsystem: meson, sources: [{type: git, url: u}]}]\n", "inline");
    CHECK(other.hash != recipe.hash);
}

TEST_CASE("invalid recipes are rejected", "[recipe]")
{
    const std::string app = std::string(APP_HEADER) + "parts: [{name: p, buildsystem: meson}]\n";
    CHECK(recipe_rejected(app + "unknown-key: 1\n"));
    CHECK(recipe_rejected(app + "kit-strategy: loose\n"));
    // Manifold refuses versions with an epoch
    CHECK(recipe_rejected(
        "type: app\ncomponent: x.y\nversion: '1:2'\nbase: c.d\nbase-version: 1\ncommand: x\n"
        "parts: [{name: p, buildsystem: meson}]\n"));

    // an app needs a command, which Manifold runs by its name
    const std::string commandless =
        "component: x.y\nversion: 1\nbase: c.d\nbase-version: 1\n"
        "parts: [{name: p, buildsystem: meson}]\n";
    CHECK(recipe_rejected(commandless));
    CHECK(recipe_rejected(commandless + "command: ''\n"));
    CHECK(recipe_rejected(commandless + "command: bin/x\n"));

    const std::string core(CORE_HEADER);
    // a core needs a bootstrap section
    CHECK(recipe_rejected(core));
    CHECK(recipe_rejected(core + "bootstrap: {suite: trixie}\n"));
    CHECK(recipe_rejected(core + std::string(CORE_BOOTSTRAP) + "locales: [de_DE.UTF-8]\n"));
    CHECK(recipe_rejected(core + std::string(CORE_BOOTSTRAP) + "locales: [de_DE, de_DE]\n"));
}

TEST_CASE("keys of other recipe types are rejected", "[recipe]")
{
    const std::string core = std::string(CORE_HEADER) + std::string(CORE_BOOTSTRAP);
    const std::string platform(PLATFORM_HEADER);
    CHECK(recipe_rejected(core + "base: c.d\n"));
    CHECK(recipe_rejected(core + "base-version: 1\n"));
    for (const auto &recipe : {core, platform}) {
        CHECK(!recipe_rejected(recipe));
        CHECK(recipe_rejected(recipe + "command: x\n"));
        CHECK(recipe_rejected(recipe + "kit-strategy: strict\n"));
        CHECK(recipe_rejected(recipe + "generate-sdk: true\n"));
        CHECK(recipe_rejected(recipe + "run-on-sdk: true\n"));
        // the default does no harm
        CHECK(!recipe_rejected(recipe + "kit-strategy: roaming\n"));
    }
}

TEST_CASE("platform recipes", "[recipe]")
{
    auto platform = parse_recipe(
        "type: platform\ncomponent: net.debian.core13.qt6\nversion: '6.8'\nname: Qt 6\nsummary: s\n"
        "base: net.debian.core13\npackages: {apt: [qt6-base-dev]}\n",
        "inline");
    CHECK(platform.is_platform());
    CHECK(!platform.is_core());
    CHECK(platform.base_version.empty());

    const std::string header(PLATFORM_HEADER);
    CHECK(recipe_rejected(header + "base-version: 1\n"));
    // a platform builds parts into its /usr, like a core
    CHECK(!recipe_rejected(header + "parts: [{name: p, buildsystem: meson}]\n"));
}

TEST_CASE("only cores have a bootstrap and actions", "[recipe]")
{
    const std::string bootstrap(CORE_BOOTSTRAP);
    const std::string actions = "actions: [{name: a, run: 'true'}]\n";
    const std::string core = std::string(CORE_HEADER) + bootstrap;
    CHECK(!recipe_rejected(core + actions));

    const std::string platform(PLATFORM_HEADER);
    const std::string app = std::string(APP_HEADER) + "parts: [{name: p, buildsystem: meson}]\n";
    for (const auto &recipe : {platform, app}) {
        CHECK(recipe_rejected(recipe + bootstrap));
        CHECK(recipe_rejected(recipe + actions));
    }
}

TEST_CASE("MetaInfo keys", "[recipe]")
{
    const std::string app = std::string(APP_HEADER) + "parts: [{name: p, buildsystem: meson}]\n";
    TempDir dir("metainfo");
    write_file(dir / "x.y.metainfo.xml", "<component/>\n");
    auto plain = parse_recipe(app, "inline", dir.path());
    CHECK(!plain.metainfo_file);
    auto named = parse_recipe(app + "metainfo-file: x.y.metainfo.xml\n", "inline", dir.path());
    CHECK(named.metainfo_file == "x.y.metainfo.xml");
    CHECK(named.hash != plain.hash);
    CHECK(recipe_rejected(app + "metainfo-file: ''\n"));
    // the file must exist, relative to the recipe
    CHECK(recipe_rejected(app + "metainfo-file: x.y.metainfo.xml\n"));
    CHECK(recipe_rejected(app + "metainfo-file: missing.metainfo.xml\n", dir.path()));

    // an app ships its own MetaInfo
    for (std::string key : {"name", "summary", "description", "project-license", "homepage"})
        CHECK(recipe_rejected(app + key + ": x\n"));

    // cores and platforms only need them to generate one
    CHECK(!recipe_rejected("type: platform\ncomponent: x.y\nversion: 1\nbase: c.d\n"));
}

TEST_CASE("invalid parts are rejected", "[recipe][parts]")
{
    const std::string app(APP_HEADER);
    // the build's HOME and DESTDIR are next to the sources
    for (std::string name : {"home", "_install"})
        CHECK(recipe_rejected(app + "parts: [{name: " + name + ", buildsystem: simple, build-commands: ['true']}]\n"));
    CHECK(recipe_rejected(app + "parts: [{name: p, buildsystem: qmake}]\n"));
    CHECK(recipe_rejected(app + "parts: [{name: p, buildsystem: simple}]\n"));
    CHECK(recipe_rejected(app + "parts: [{name: p, buildsystem: meson}, {name: p, buildsystem: meson}]\n"));
    CHECK(recipe_rejected(app + "parts: [{name: a/b, buildsystem: meson}]\n"));
    CHECK(recipe_rejected(app + "parts: [{name: p, buildsystem: meson, subdir: ../x}]\n"));
    // post-install must be a list of commands, like in Flatpak
    CHECK(recipe_rejected(app + "parts: [{name: p, buildsystem: meson, post-install: 'true'}]\n"));

    auto with_source = [&](std::string source) {
        return recipe_rejected(app + "parts: [{name: p, buildsystem: meson, sources: [" + source + "]}]\n");
    };
    CHECK(with_source("{type: svn, url: u}"));
    CHECK(with_source("{type: git}"));
    CHECK(with_source("{type: git, url: u, sha256: x}"));
    CHECK(with_source("{type: archive, url: 'https://x/a.tar.xz'}"));
    CHECK(with_source("{type: archive, url: u, path: p, sha256: x}"));
    CHECK(with_source("{type: file, url: u}"));
    CHECK(with_source("{type: file, path: p, dest-filename: a/b}"));
    CHECK(with_source("{type: dir}"));
    CHECK(with_source("{type: patch, url: u}"));
    CHECK(with_source("{type: dir, path: d, dest: ../outside}"));
    CHECK(!with_source("{type: archive, url: 'https://x/a.tar.xz', sha256: x, strip-components: 2}"));
    CHECK(!with_source("{type: archive, path: a.zip}"));
    CHECK(!with_source("{type: file, path: p, dest-filename: q}"));
    CHECK(!with_source("{type: dir, path: d, dest: sub/dir}"));
    CHECK(!with_source("{type: patch, path: fix.patch, strip-components: 0}"));
}

TEST_CASE("parts are included from files and nested", "[recipe][parts]")
{
    TempDir dir("includes");
    fs::create_directories(dir / "parts");
    write_file(
        dir / "parts/lib.yaml",
        "name: lib\nbuildsystem: meson\nsources: [{type: dir, path: lib-src}]\nparts: [dep.yaml]\n");
    write_file(dir / "parts/dep.yaml", "name: dep\nbuildsystem: cmake\n");
    write_file(
        dir / "app.yaml",
        std::string(APP_HEADER)
            + "parts:\n"
              "- parts/lib.yaml\n"
              "- name: app\n"
              "  buildsystem: meson\n"
              "  parts:\n"
              "  - {name: inner, buildsystem: simple, build-commands: [make], only-arches: [aarch64]}\n"
              "  - {name: other, buildsystem: simple, build-commands: [make], skip-arches: [aarch64]}\n");

    auto recipe = load_recipe(dir / "app.yaml");
    REQUIRE(recipe.parts.size() == 2);
    const auto &lib = std::get<Part>(recipe.parts[0]);
    CHECK(lib.name == "lib");
    // local sources of an included part are relative to its own file
    CHECK(lib.dir == fs::canonical(dir / "parts"));
    CHECK(std::get<Part>(lib.parts[0]).dir == fs::canonical(dir / "parts"));
    CHECK(std::get<Part>(recipe.parts[1]).dir == fs::absolute(dir.path()));

    // nested parts are built first, and arch filters drop parts
    CHECK(names(parts_to_build(recipe, "x86_64")) == std::vector<std::string>{"dep", "lib", "other", "app"});
    CHECK(names(parts_to_build(recipe, "aarch64")) == std::vector<std::string>{"dep", "lib", "inner", "app"});

    // the hash covers the included files
    auto hash = recipe.hash;
    write_file(dir / "parts/dep.yaml", "name: dep\nbuildsystem: cmake\nconfig-opts: [-DX=1]\n");
    CHECK(load_recipe(dir / "app.yaml").hash != hash);

    // part names must be unique across files, and include cycles are refused
    write_file(dir / "parts/dep.yaml", "name: lib\nbuildsystem: cmake\n");
    CHECK_THROWS_AS(load_recipe(dir / "app.yaml"), BuildError);
    write_file(dir / "parts/dep.yaml", "name: dep\nbuildsystem: cmake\nparts: [lib.yaml]\n");
    CHECK_THROWS_AS(load_recipe(dir / "app.yaml"), BuildError);
}

TEST_CASE("build-options combine", "[recipe][parts]")
{
    auto recipe = parse_recipe(
        std::string(APP_HEADER)
            + "build-options:\n"
              "  cflags: -O2\n"
              "  env: {A: global, B: global}\n"
              "  prepend-path: /global/bin\n"
              "  config-opts: [-Dglobal=true]\n"
              "  arch:\n"
              "    x86_64: {cflags: -march=x86-64-v2, env: {B: global-x86}}\n"
              "parts:\n"
              "- name: p\n"
              "  buildsystem: meson\n"
              "  build-options:\n"
              "    cflags: -g\n"
              "    ldflags: -Wl,--as-needed\n"
              "    env: {A: part}\n"
              "    config-opts: [-Dpart=true]\n"
              "    arch: {aarch64: {cflags: -mbranch-protection=standard}}\n",
        "inline");
    const auto &part = std::get<Part>(recipe.parts[0]);

    auto x86 = merge_build_settings(recipe, part, "x86_64");
    CHECK(x86.cflags == "-O2 -march=x86-64-v2 -g");
    CHECK(x86.ldflags == "-Wl,--as-needed");
    CHECK(x86.env.at("A") == "part");
    CHECK(x86.env.at("B") == "global-x86");
    CHECK(x86.prepend_path == "/global/bin");
    CHECK(x86.config_opts == std::vector<std::string>{"-Dglobal=true", "-Dpart=true"});

    auto arm = merge_build_settings(recipe, part, "aarch64");
    CHECK(arm.cflags == "-O2 -g -mbranch-protection=standard");
    CHECK(arm.env.at("B") == "global");
}

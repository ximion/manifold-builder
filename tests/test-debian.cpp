/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cores/core-system.hpp"
#include "cores/debian/debian.hpp"
#include "cores/debian/dpkg.hpp"
#include "image.hpp"
#include "sandbox.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <manifold.h>

using namespace mfb;
using mfb::test::APP_HEADER;
using mfb::test::TempDir;

namespace
{

DebianSystem trixie(std::string architecture = "amd64")
{
    return DebianSystem("/nonexistent", Bootstrap{.suite = "trixie", .architecture = std::move(architecture)}, 0);
}

} // namespace

TEST_CASE("architecture names", "[debian]")
{
    CHECK(manifold_architecture("amd64") == "x86_64");
    CHECK(manifold_architecture("arm64") == "aarch64");
    CHECK(dpkg_architecture("x86_64") == "amd64");
    CHECK(repart_architecture("x86_64") == "x86-64");
    CHECK(repart_architecture("aarch64") == "arm64");
    // 32-bit ARM is named by ABI, since armhf and armel share the CPU part of the tuple
    CHECK(manifold_architecture("armhf") == "armhf");
    CHECK(dpkg_architecture("armel") == "armel");
    CHECK(repart_architecture("armhf") == "arm");
    CHECK(arch_name_for_filename("x86_64") == "x86-64");
    CHECK(arch_name_for_filename("aarch64") == "aarch64");

    CHECK(trixie().multiarch() == "x86_64-linux-gnu");
    CHECK(trixie("armhf").multiarch() == "arm-linux-gnueabihf");
    CHECK(trixie().architecture() == "x86_64");
    // without an architecture, we build for the host
    CHECK(trixie("").architecture() == mf_get_host_architecture());
}

TEST_CASE("the bootstrap section of a Debian core", "[debian]")
{
    CHECK(DebianSystem::check_bootstrap({.suite = "trixie", .snapshot = "2026-01-04T00:00:00Z"}).empty());
    CHECK(!DebianSystem::check_bootstrap({.suite = "", .snapshot = "x"}).empty());
    CHECK(!DebianSystem::check_bootstrap({.suite = "trixie"}).empty());
    CHECK(!DebianSystem::check_bootstrap({.suite = "trixie", .architecture = "x86_64", .snapshot = "x"}).empty());
    CHECK(!check_bootstrap({.distribution = "fedora", .suite = "43", .snapshot = "x"}).empty());
}

TEST_CASE("the slim configuration keeps the catalogs of the locales", "[debian]")
{
    Script s;
    trixie().slimify(s, "/tree", {"de_DE", "pt_BR"});
    const auto &text = s.text();
    CHECK(text.find("path-include /usr/share/locale/de/*") != std::string::npos);
    CHECK(text.find("path-include /usr/share/locale/pt_BR/*") != std::string::npos);
    CHECK(text.find("! -name de_DE") != std::string::npos);
    CHECK(text.find("path-exclude /usr/share/man/*") != std::string::npos);
}

TEST_CASE("commands in a tree run without questions", "[debian]")
{
    Script s;
    trixie().run_in_tree(s, "/tree", "apt-get install -y foo");
    CHECK(s.text().find("DEBIAN_FRONTEND=noninteractive") != std::string::npos);
    CHECK(s.text().find("umask 022") != std::string::npos);
}

TEST_CASE("package installs never remove packages and share their downloads", "[debian]")
{
    Script s;
    auto debian = trixie();
    debian.install(s, "/tree", {"foo"});
    CHECK(s.text().find("--no-remove") != std::string::npos);
    CHECK(s.text().find("Dir::Cache::Archives=/" + debian.package_cache_dir()) != std::string::npos);
    // the shared package cache belongs to us, not to apt's sandbox user
    CHECK(s.text().find("APT::Sandbox::User=root") != std::string::npos);
    // debuerreotype empties apt's own archives directory after every dpkg run
    CHECK(debian.package_cache_dir() != "var/cache/apt/archives");
}

TEST_CASE("dpkg status files", "[debian]")
{
    auto packages = parse_dpkg_status(
        "Package: zlib1g\n"
        "Status: install ok installed\n"
        "Version: 1:1.3.1-1\n"
        "\n"
        "Package: removed\n"
        "Status: deinstall ok config-files\n"
        "Version: 1.0\n"
        "\n"
        "Package: apt\n"
        "Status: install ok installed\n"
        "Version: 3.0.3\n");
    REQUIRE(packages.size() == 2);
    CHECK(packages.begin()->first == "apt");
    CHECK(packages["zlib1g"] == "1:1.3.1-1");
}

TEST_CASE("apt-cache depends output", "[debian]")
{
    auto graph = parse_apt_cache_depends(
        "qt6-base-dev\n"
        "  Depends: libqt6core6t64\n"
        " |Depends: libgl-dev\n"
        "  Depends: libgl1-mesa-dev\n"
        "  Depends: <libvirtual>\n"
        "    libprovider1\n"
        "    libprovider2\n"
        "  PreDepends: dpkg\n"
        "libqt6core6t64\n"
        "  Depends: libc6\n");
    CHECK(graph["qt6-base-dev"] == std::vector<std::string>{"libqt6core6t64", "libgl-dev", "libprovider1", "dpkg"});
    CHECK(graph["libqt6core6t64"] == std::vector<std::string>{"libc6"});
}

TEST_CASE("the runtime takes what the development packages depend on", "[debian]")
{
    TempDir dir("runtime-request");
    write_file(
        dir / "depends.txt",
        "libfoo-dev\n"
        "  Depends: libfoo1\n"
        "  Depends: libbar-dev\n"
        "  Depends: pkgconf\n"
        "libbar-dev\n"
        "  Depends: libbar2\n");
    auto recipe = parse_recipe(
        std::string(APP_HEADER)
            + "packages: {apt: [libfoo-dev, fontconfig]}\n"
              "packages-sdk: {apt: [pkgconf, meson]}\n"
              "parts: [{name: p, buildsystem: meson}]\n",
        "inline");
    auto debian = trixie();
    CHECK(debian.development_packages(recipe) == std::vector<std::string>{"libfoo-dev"});
    CHECK(
        debian.packages_request_runtime(recipe, dir / "depends.txt")
        == std::vector<std::string>{"fontconfig", "libbar2", "libfoo1"});
    CHECK(
        debian.packages_request_sdk(recipe)
        == std::vector<std::string>{"fontconfig", "libfoo-dev", "meson", "pkgconf"});

    // the recipe's own patterns replace the distribution's
    auto own = parse_recipe(
        std::string(APP_HEADER)
            + "packages: {apt: [libfoo-dev, foo-tools]}\nsdk-patterns: ['*-tools']\n"
              "parts: [{name: p, buildsystem: meson}]\n",
        "inline");
    CHECK(debian.development_packages(own) == std::vector<std::string>{"foo-tools"});

    // packages of a different package system are refused
    auto fedora = parse_recipe(
        std::string(APP_HEADER) + "packages: {dnf: [foo-devel]}\nparts: [{name: p, buildsystem: meson}]\n", "inline");
    CHECK_THROWS_AS(debian.check_packages(fedora), BuildError);
    CHECK_NOTHROW(debian.check_packages(recipe));
}

TEST_CASE("locales bring the packages that generate them", "[debian]")
{
    TempDir dir("locales");
    write_file(dir / "depends.txt", "");
    auto core = parse_recipe(
        "type: core\ncomponent: x.y\nversion: 1\nname: x\nsummary: x\n"
        "bootstrap: {suite: trixie, snapshot: '2026-01-04T00:00:00Z'}\n"
        "locales: [de_DE]\npackages: {apt: [fontconfig]}\n",
        "inline");
    CHECK(
        trixie().packages_request_runtime(core, dir / "depends.txt")
        == std::vector<std::string>{"fontconfig", "libc-l10n", "locales"});
}

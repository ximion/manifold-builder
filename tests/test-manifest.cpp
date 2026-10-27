/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "appstream.hpp"
#include "checksum.hpp"
#include "identity.hpp"
#include "image.hpp"
#include "manifest.hpp"
#include "testshared.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

using namespace mfb;
using mfb::test::TempDir;

TEST_CASE("file and string hashes", "[hashes]")
{
    TempDir dir("hashes");
    write_file(dir / "empty", "");
    CHECK(
        checksum_file_hex(dir / "empty", ChecksumKind::Sha256)
        == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(
        checksum_file_hex(dir / "empty", ChecksumKind::Blake3)
        == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    // known b3sum of the empty input, to check the encoding
    CHECK(
        checksum_data_hex("", ChecksumKind::Blake3)
        == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    // the first 128 bits, with version and variant set
    CHECK(
        image_uuid("af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262")
        == "af1349b9-f5f9-81a6-a040-4dea36dcc949");

    // both at once, over more than one chunk
    write_file(dir / "large", std::string(3 << 20, 'x'));
    auto both = checksum_file(dir / "large");
    CHECK(both.blake3 == checksum_file_hex(dir / "large", ChecksumKind::Blake3));
    CHECK(both.sha256 == checksum_file_hex(dir / "large", ChecksumKind::Sha256));
    CHECK(both.sha256 == checksum_data_hex(std::string(3 << 20, 'x'), ChecksumKind::Sha256));
    // a directory is no empty file
    CHECK_THROWS_AS(checksum_file(dir.path()), BuildError);
    CHECK_THROWS_AS(checksum_file_hex(dir.path(), ChecksumKind::Sha256), BuildError);
}

TEST_CASE("identity and content hashes", "[hashes]")
{
    InstalledPackages packages = {{"apt", "3.0.3"}, {"zlib1g", "1:1.3.1-1"}};
    InstalledPackages upgraded = {{"apt", "3.0.4"}, {"zlib1g", "1:1.3.1-1"}};
    InstalledPackages added = {{"apt", "3.0.3"}, {"bash", "5.2"}, {"zlib1g", "1:1.3.1-1"}};

    auto identity = identity_hash("net.debian.core13", "x86_64", packages);
    CHECK(identity == identity_hash("net.debian.core13", "x86_64", upgraded));
    CHECK(identity != identity_hash("net.debian.core13", "x86_64", added));
    CHECK(identity != identity_hash("net.debian.core13", "arm64", packages));
    CHECK(
        identity
        == checksum_data_hex(
            "manifold-identity/1\nbase: net.debian.core13\narchitecture: x86_64\napt\nzlib1g\n", ChecksumKind::Blake3));

    auto content = content_hash("net.debian.core13", "x86_64", "net.debian.core13", "13.1", packages);
    CHECK(content != content_hash("net.debian.core13", "x86_64", "net.debian.core13", "13.1", upgraded));
    CHECK(content != content_hash("net.debian.core13", "x86_64", "net.debian.core13", "13.1", packages, {"recipe: x"}));
}

TEST_CASE("directory tree hashes", "[hashes]")
{
    TempDir dir("tree-hash");
    fs::create_directories(dir / "a/sub");
    write_file(dir / "a/sub/file", "content");
    write_file(dir / "a/script", "#!/bin/sh\n");
    fs::create_symlink("sub/file", dir / "a/link");
    auto hash = tree_hash(dir / "a");
    CHECK(hash.size() == 64);

    // a copy hashes the same
    fs::copy(dir / "a", dir / "b", fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    CHECK(tree_hash(dir / "b") == hash);

    fs::permissions(dir / "b/script", fs::perms::owner_exec, fs::perm_options::add);
    auto executable = tree_hash(dir / "b");
    CHECK(executable != hash);
    write_file(dir / "b/sub/file", "changed");
    CHECK(tree_hash(dir / "b") != executable);
}

TEST_CASE("Manifold refuses invalid hashes, so they are never written", "[manifest]")
{
    TempDir dir("bad-hash");
    Manifest manifest;
    manifest.identity = checksum_data_hex("x", ChecksumKind::Blake3);
    manifest.content_hash = "AF1349B9";
    CHECK_THROWS_AS(write_manifest(manifest, dir / "manifest.json"), BuildError);
    CHECK(!fs::exists(dir / "manifest.json"));
}

TEST_CASE("file names of images and bundles", "[manifest]")
{
    CHECK(
        image_file_name("net.debian.core13.sdk", "13.1", 22880949, "x86_64", ".raw")
        == "net.debian.core13.sdk_13.1-22880949_x86-64.raw");
    Manifest manifest{.component = "a", .version = "1", .revision = 2, .architecture = "aarch64"};
    CHECK(image_file_name(manifest, ".manifold.json") == "a_1-2_aarch64.manifold.json");
    // a bundle is named without the revision
    CHECK(bundle_file_name("com.example.App", "1.0", "x86_64") == "com.example.App_1.0_x86-64.manifold");
}

TEST_CASE("manifests round-trip through JSON", "[manifest]")
{
    Manifest manifest;
    manifest.component = "net.debian.core13";
    manifest.kind = "core";
    manifest.version = "13.1";
    manifest.architecture = "x86_64";
    manifest.packages["apt"] = {{"apt", "3.0.3"}};
    manifest.build.tools["mkfs.erofs"] = "1.9.4";
    manifest.sources = std::vector<SourceRecord>{
        {.part = "a", .type = "git", .url = "https://example.org/a.git", .commit = "0123"},
        {.part = "b", .type = "dir", .path = "b-src", .tree_hash = "abcd"},
    };

    auto json = to_json(manifest);
    CHECK(json.find("\"content-hash\"") != std::string::npos);
    CHECK(json.find("\"image\"") == std::string::npos);
    CHECK(json.find("\"apt\": {") != std::string::npos);
    // a source only records the keys it has
    CHECK(json.find("\"sha256\"") == std::string::npos);
    CHECK(json.find("\"tree-hash\": \"abcd\"") != std::string::npos);

    auto back = manifest_from_json(json, "roundtrip");
    CHECK(back.component == manifest.component);
    CHECK(back.packages == manifest.packages);
    REQUIRE(back.sources);
    CHECK(back.sources->at(0).pin() == "0123");
    CHECK(back.sources->at(1).pin() == "abcd");
    CHECK(!back.sources->at(1).url);
}

TEST_CASE("generated MetaInfo validates", "[metainfo]")
{
    MetaInfo sdk{
        .id = "net.example.core.sdk",
        .kind = "addon",
        .extends = "net.example.core",
        .name = "Example Core (SDK)",
        .summary = "Development files for Example Core",
        .description = "First paragraph\nstill first.\n\nSecond <paragraph>.\n",
        .project_license = "MIT",
        .metadata_license = "CC0-1.0",
        .homepage = "https://example.org/core",
        .version = "1.0",
        .release_date = "2026-01-04",
    };
    auto xml = generate_metainfo(sdk);
    CHECK(xml.find("type=\"addon\"") != std::string::npos);
    CHECK(xml.find("<extends>net.example.core</extends>") != std::string::npos);
    CHECK(xml.find("<p>First paragraph still first.</p>") != std::string::npos);
    CHECK(xml.find("Second &lt;paragraph&gt;") != std::string::npos);
    // only the MetaInfo in the bundle names the image to install
    CHECK(xml.find("manifold.id") == std::string::npos);
    auto problems = validate_metainfo(xml);
    for (const auto &problem : problems)
        UNSCOPED_INFO("validator: " << problem.text);
    CHECK(problems.empty());

    MetaInfo runtime = sdk;
    runtime.id = "net.example.core";
    runtime.kind = "runtime";
    runtime.extends.clear();
    auto runtime_xml = generate_metainfo(runtime);
    CHECK(runtime_xml.find("type=\"runtime\"") != std::string::npos);
    auto runtime_problems = validate_metainfo(runtime_xml);
    for (const auto &problem : runtime_problems)
        UNSCOPED_INFO("validator: " << problem.text);
    CHECK(runtime_problems.empty());

    // an application needs a description, and should have a content rating
    const std::string app = R"(<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>com.example.Broken</id>
  <name>Broken</name>
  <summary>Has no description</summary>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>MIT</project_license>
</component>
)";
    auto issues = validate_metainfo(app);
    CHECK(std::ranges::contains(issues, true, &MetaInfoIssue::error));
    CHECK(std::ranges::contains(issues, false, &MetaInfoIssue::error));
}

TEST_CASE("MetaInfo of an image goes into the catalog of the bundle", "[metainfo]")
{
    TempDir dir("metainfo");
    auto tree = dir / "tree";
    auto file = tree / "usr/share/metainfo/net.example.core.metainfo.xml";
    fs::create_directories(file.parent_path());
    write_file(
        file,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<component type="runtime">
  <id>net.example.core</id>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>MIT</project_license>
  <name>Example Core</name>
  <name xml:lang="de">Beispielkern</name>
  <summary>Base layer for examples</summary>
  <url type="homepage">https://example.org/core</url>
  <icon type="stock">net.example.core</icon>
</component>
)");
    // a package of the core with MetaInfo of its own
    write_file(
        tree / "usr/share/metainfo/org.example.tool.metainfo.xml",
        R"(<?xml version="1.0" encoding="UTF-8"?>
<component type="console-application">
  <id>org.example.tool</id>
  <metadata_license>CC0-1.0</metadata_license>
  <name>Example Tool</name>
  <summary>A tool in the core</summary>
</component>
)");
    auto icon = tree / "usr/share/icons/hicolor/scalable/apps/net.example.core.svg";
    fs::create_directories(icon.parent_path());
    write_file(
        icon,
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="#3584e4"/></svg>)");

    auto info = parse_metainfo(read_file(file));
    CHECK(info.id == "net.example.core");
    CHECK(info.kind == "runtime");
    CHECK(info.name == "Example Core");
    CHECK(info.summary == "Base layer for examples");
    CHECK(info.project_license == "MIT");
    CHECK(info.metadata_license == "CC0-1.0");
    CHECK(info.homepage == "https://example.org/core");

    // the catalog keeps the translations and the icon, and names the image to install
    AppStreamCatalog catalog(dir / "catalog");
    catalog.add_tree(tree, "/usr", "net.example.core", manifold_custom("x86_64", "net.example.core", "1.0"));
    auto catalog_file = catalog.write();
    CHECK(catalog_file == dir / "catalog/manifold-local.xml");
    auto xml = read_file(catalog_file);
    CHECK(xml.find("<components ") != std::string::npos);
    CHECK(xml.find("origin=\"manifold-local\"") != std::string::npos);
    CHECK(xml.find("Beispielkern") != std::string::npos);
    CHECK(xml.find("<value key=\"manifold.id\">x86_64/net.example.core/1.0</value>") != std::string::npos);
    CHECK(xml.find("<icon type=\"cached\"") != std::string::npos);
    CHECK(fs::is_regular_file(dir / "catalog/icons/64x64/net.example.core.jxl"));
    CHECK(xml.find("org.example.tool") == std::string::npos);
    CHECK(read_file(file).find("manifold.id") == std::string::npos);

    // the tree has to describe the image it belongs to
    CHECK_THROWS_AS(catalog.add_tree(tree, "/usr", "net.example.other", {}), BuildError);
}

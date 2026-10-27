/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "base-store.hpp"
#include "checksum.hpp"
#include "manifest.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

using namespace mfb;
using mfb::test::TempDir;

TEST_CASE("cores and platforms are found", "[store]")
{
    TempDir tmp("core-lookup");
    const auto &dir = tmp.path();
    // made-up hashes that still look valid, so Manifold accepts the manifests
    auto image_of = [&](const std::string &file) {
        std::ofstream(dir / file) << "not really an image";
        return ImageFile{
            .file = file,
            .size = fs::file_size(dir / file),
            .blake3 = checksum_file_hex(dir / file, ChecksumKind::Blake3),
            .sha256 = checksum_file_hex(dir / file, ChecksumKind::Sha256),
            .verity_root_hash = checksum_data_hex("verity " + file, ChecksumKind::Sha256),
            .data_offset = 1048576,
            .hash_offset = 2097152,
        };
    };
    auto describe = [&](std::string version, bool sdk, int64_t revision = 1, std::string build = "") {
        Manifest manifest;
        manifest.component = sdk ? "net.example.core.sdk" : "net.example.core";
        manifest.kind = "core";
        manifest.version = version;
        manifest.architecture = "x86_64";
        manifest.sdk = sdk;
        manifest.identity = checksum_data_hex(sdk ? "sdk-identity" : "core-identity", ChecksumKind::Sha256);
        manifest.content_hash = checksum_data_hex(version + build + (sdk ? "-sdk" : ""), ChecksumKind::Sha256);
        if (sdk)
            manifest.base = BaseRef{
                .component = "net.example.core",
                .version = version,
                .identity = checksum_data_hex("core-identity", ChecksumKind::Sha256),
                .content_hash = checksum_data_hex(version + build, ChecksumKind::Sha256)};
        manifest.revision = revision;
        auto stem = std::format("{}_{}-{}_x86-64", manifest.component, version, revision);
        manifest.image = image_of(stem + ".raw");
        write_manifest(manifest, dir / (stem + ".manifold.json"));
    };
    // a platform built on core 12.2
    auto platform = [&](bool sdk) {
        Manifest manifest;
        manifest.component = sdk ? "net.example.platform.sdk" : "net.example.platform";
        manifest.kind = "platform";
        manifest.version = "20260117.0900";
        manifest.architecture = "x86_64";
        manifest.sdk = sdk;
        manifest.identity = checksum_data_hex("platform-identity", ChecksumKind::Sha256);
        manifest.content_hash = checksum_data_hex(sdk ? "p-sdk" : "p", ChecksumKind::Sha256);
        auto core_ref = BaseRef{
            .component = "net.example.core",
            .version = "12.2",
            .identity = checksum_data_hex("core-identity", ChecksumKind::Sha256),
            .content_hash = checksum_data_hex("12.2", ChecksumKind::Sha256)};
        auto platform_ref = BaseRef{
            .component = "net.example.platform",
            .version = manifest.version,
            .identity = checksum_data_hex("platform-identity", ChecksumKind::Sha256),
            .content_hash = checksum_data_hex("p", ChecksumKind::Sha256)};
        manifest.base = sdk ? platform_ref : core_ref;
        manifest.image = image_of(std::format("{}_{}-0_x86-64.raw", manifest.component, manifest.version));
        write_manifest(
            manifest, dir / std::format("{}_{}-0_x86-64.manifold.json", manifest.component, manifest.version));
    };
    for (auto version : {"12.1a", "12.2", "12.10"}) {
        describe(version, false);
        describe(version, true);
    }
    describe("12.10", false, 2);
    describe("12.10", true, 2);
    platform(false);
    platform(true);
    // a newer build of the core version the platform was built on
    describe("12.2", false, 2, "rebuilt");
    describe("12.2", true, 2, "rebuilt");

    // Manifold's own store layout, read through libmanifold. Images are stored by
    // verity hash. Each component has a directory per version, holding the manifest
    // and a `runtime.raw` link to the image, and `current` points at the active
    // version. A `base` link points to the version directory of the layer below.
    auto store = dir / "store";
    auto install =
        [&](const std::string &component, const std::string &full_version, const std::string &base_link = "") {
            auto stem = std::format("{}_{}_x86-64", component, full_version);
            auto manifest = manifest_from_json(read_file(dir / (stem + ".manifold.json")), "");
            auto verity = manifest.image->verity_root_hash;
            auto version_dir = store / "runtimes" / component / full_version;
            fs::create_directories(store / "images/by-verity");
            fs::create_directories(version_dir);
            fs::copy_file(dir / (stem + ".raw"), store / "images/by-verity" / (verity + ".raw"));
            fs::create_symlink("../../../images/by-verity/" + verity + ".raw", version_dir / "runtime.raw");
            if (!base_link.empty())
                fs::create_symlink(base_link, version_dir / "base");
            write_manifest(manifest, version_dir / "manifest.json");
            fs::create_symlink(full_version, store / "runtimes" / component / "current");
            return store / "images/by-verity" / (verity + ".raw");
        };

    SECTION("the newest version satisfying the minimum wins, with its newest revision")
    {
        // in Manifold's ordering, 12.10 is newer than 12.2
        auto core = find_stack(dir, "net.example.core", "12.2").core;
        CHECK(core.runtime.version == "12.10");
        CHECK(core.sdk.component == "net.example.core.sdk");
        CHECK(core.sdk.version == "12.10");
        CHECK(core.runtime_image.filename() == "net.example.core_12.10-2_x86-64.raw");
        CHECK(core.sdk_image.filename() == "net.example.core.sdk_12.10-2_x86-64.raw");
        CHECK_THROWS_AS(find_stack(dir, "net.example.core", "14"), BuildError);
    }

    SECTION("a platform uses the exact core it was built on, not the newest")
    {
        auto on_platform = find_stack(dir, "net.example.platform", "");
        REQUIRE(on_platform.platform);
        CHECK(on_platform.base().runtime.component == "net.example.platform");
        CHECK(on_platform.core.runtime.version == "12.2");
        CHECK(on_platform.core.runtime.revision == 1);
        CHECK(on_platform.core.sdk.version == "12.2");
        CHECK(on_platform.core.sdk.revision == 1);
    }

    SECTION("a store is read like Manifold does")
    {
        auto core_image = install("net.example.core", "12.2-1");
        auto sdk_image = install("net.example.core.sdk", "12.2-1");
        auto installed = find_stack(store, "net.example.core", "12.1").core;
        CHECK(installed.runtime.version == "12.2");
        CHECK(installed.runtime_image == core_image);
        // the SDK of the same version, found through its own directory
        CHECK(installed.sdk_image == sdk_image);
        CHECK_THROWS_AS(find_stack(store, "net.example.core", "12.3"), BuildError);
    }

    SECTION("an installed platform uses the core its base link points to")
    {
        auto core_image = install("net.example.core", "12.2-1");
        auto sdk_image = install("net.example.core.sdk", "12.2-1");
        install("net.example.platform", "20260117.0900-0", "../../../runtimes/net.example.core/12.2-1");
        install(
            "net.example.platform.sdk", "20260117.0900-0", "../../../runtimes/net.example.platform/20260117.0900-0");
        auto installed_platform = find_stack(store, "net.example.platform", "");
        REQUIRE(installed_platform.platform);
        CHECK(installed_platform.base().runtime.component == "net.example.platform");
        CHECK(installed_platform.core.runtime_image == core_image);
        CHECK(installed_platform.core.sdk_image == sdk_image);
    }
}

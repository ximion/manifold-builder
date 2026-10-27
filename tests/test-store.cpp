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

using namespace mfb;
using mfb::test::TempDir;

TEST_CASE("cores and platforms are found", "[store]")
{
    TempDir tmp("core-lookup");
    const auto &dir = tmp.path();
    // Write the manifests of a layer and its SDK as the builder names them, and
    // return the layer's reference. `build` tells rebuilds of a version apart.
    auto write_layer = [&](const std::string &component,
                           const std::string &kind,
                           const std::string &version,
                           int64_t revision,
                           const std::string &build = "",
                           std::optional<BaseRef> base = std::nullopt) {
        auto ref = [&](bool sdk) {
            auto id = component + (sdk ? ".sdk" : "");
            return BaseRef{
                .component = id,
                .version = version,
                .identity = checksum_data_hex(id, ChecksumKind::Sha256),
                .content_hash = checksum_data_hex(id + version + build, ChecksumKind::Sha256),
            };
        };
        for (bool sdk : {false, true}) {
            auto self = ref(sdk);
            Manifest manifest{
                .component = self.component,
                .kind = kind,
                .version = version,
                .revision = revision,
                .architecture = "x86_64",
                .sdk = sdk,
                .identity = self.identity,
                .content_hash = self.content_hash,
                .base = sdk ? ref(false) : base,
            };
            auto stem = std::format("{}_{}-{}_x86-64", manifest.component, version, revision);
            manifest.image = test::fake_image(dir, stem + ".raw");
            write_manifest(manifest, dir / (stem + ".manifold.json"));
        }
        return ref(false);
    };
    write_layer("net.example.core", "core", "12.1a", 1);
    auto core = write_layer("net.example.core", "core", "12.2", 1);
    write_layer("net.example.core", "core", "12.10", 1);
    write_layer("net.example.core", "core", "12.10", 2);
    // a platform built on core 12.2
    write_layer("net.example.platform", "platform", "20260117.0900", 0, "", core);
    // a newer build of the core version the platform was built on
    write_layer("net.example.core", "core", "12.2", 2, "rebuilt");

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

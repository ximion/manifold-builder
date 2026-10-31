/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "bundle.hpp"
#include "checksum.hpp"
#include "manifest.hpp"
#include "process.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <zip.h>

using namespace mfb;
using Catch::Matchers::ContainsSubstring;
using mfb::test::TempDir;

namespace
{

void replace_entry(const fs::path &archive_path, const char *name, std::string_view data)
{
    zip_t *archive = zip_open(archive_path.c_str(), 0, nullptr);
    zip_source_t *changed = zip_source_buffer(archive, data.data(), data.size(), 0);
    zip_file_add(archive, name, changed, ZIP_FL_OVERWRITE);
    zip_close(archive);
}

/**
 * Write a bundle with one fake image, its manifest and a catalog that
 * Manifold accepts, and return its path.
 */
fs::path make_test_bundle(const fs::path &dir, MfSigner *signer = nullptr)
{
    Manifest manifest;
    manifest.component = "a";
    manifest.kind = "core";
    manifest.version = "1";
    manifest.revision = 1;
    manifest.architecture = "x86_64";
    manifest.identity = checksum_data_hex("a", ChecksumKind::Sha256);
    manifest.content_hash = checksum_data_hex("a 1", ChecksumKind::Sha256);
    manifest.image = test::fake_image(dir, "a.raw");
    auto catalog = R"(<?xml version="1.0" encoding="UTF-8"?>
<components version="1.0">
  <component type="generic">
    <id>a</id>
    <name>a</name>
  </component>
</components>
)";
    fs::create_directories(dir / "icons");
    fs::create_directories(dir / "media");
    auto bundle = dir / "a_1.manifold";
    write_bundle(bundle, {{dir / "a.raw", to_json(manifest), catalog}}, dir / "icons", dir / "media", signer);
    return bundle;
}

} // namespace

TEST_CASE("Manifold accepts a bundle, but not once its image changed", "[bundle]")
{
    TempDir dir("bundle");
    auto bundle = make_test_bundle(dir.path());

    CHECK_NOTHROW(verify_bundle(bundle));
    replace_entry(bundle, "a.raw", "not really an imagf");
    CHECK_THROWS_AS(verify_bundle(bundle), BuildError);
}

TEST_CASE("only a key that was asked for has to exist", "[bundle]")
{
    // the test run points XDG_CONFIG_HOME at an empty directory
    CHECK(select_signer(nullptr) == nullptr);
    CHECK_THROWS_AS(select_signer("no-such-key"), BuildError);
}

TEST_CASE("a bundle that cannot be signed is written unsigned", "[bundle]")
{
    if (!find_tool("ssh-keygen"))
        SKIP("ssh-keygen is not installed");
    TempDir dir("sign-fails");
    write_file(dir / "key", "not a key\n");
    g_autoptr(MfSigner) signer = select_signer((dir / "key").c_str());

    CHECK_THROWS_WITH(make_test_bundle(dir.path(), signer), ContainsSubstring("was written unsigned"));
    g_autoptr(MfBundle) bundle = mf_bundle_new_from_path((dir / "a_1.manifold").c_str(), nullptr);
    REQUIRE(bundle != nullptr);
    CHECK(!mf_bundle_is_signed(bundle));
}

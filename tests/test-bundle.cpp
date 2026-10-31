/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "bundle.hpp"
#include "checksum.hpp"
#include "manifest.hpp"
#include "process.hpp"
#include "scopeguard.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <zip.h>

using namespace mfb;
using Catch::Matchers::ContainsSubstring;
using mfb::test::TempDir;

namespace
{

/**
 * Read one file from a ZIP archive, or return an empty string if it is missing.
 */
std::string zip_entry(const fs::path &archive_path, const char *name)
{
    zip_t *archive = zip_open(archive_path.c_str(), ZIP_RDONLY, nullptr);
    REQUIRE(archive != nullptr);
    std::string data;
    zip_stat_t stat;
    if (zip_stat(archive, name, 0, &stat) == 0) {
        zip_file_t *file = zip_fopen(archive, name, 0);
        data.resize(stat.size);
        zip_fread(file, data.data(), data.size());
        zip_fclose(file);
    }
    zip_close(archive);
    return data;
}

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
fs::path make_test_bundle(const fs::path &dir)
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
    write_manifest(manifest, dir / "a.manifold.json");
    write_file(
        dir / "catalog.xml",
        R"(<?xml version="1.0" encoding="UTF-8"?>
<components version="1.0" origin="manifold-local">
  <component type="generic">
    <id>a</id>
    <name>a</name>
    <bundle type="manifold">x86_64/a/1</bundle>
  </component>
</components>
)");
    auto bundle = dir / "a_1.manifold";
    write_bundle(
        bundle,
        {
            {"a.raw", dir / "a.raw", true},
            {"a.manifold.json", dir / "a.manifold.json", false},
            {"catalog/manifold-local.xml", dir / "catalog.xml", false},
        });
    return bundle;
}

} // namespace

TEST_CASE("bundles carry an index of their manifests and catalog", "[bundle]")
{
    TempDir dir("bundle");
    auto bundle = make_test_bundle(dir.path());

    auto index_json = zip_entry(bundle, "index.json");
    REQUIRE(!index_json.empty());
    CHECK(zip_entry(bundle, "index.json.sig").empty());
    // images are not listed, their manifests are
    CHECK_THAT(index_json, !ContainsSubstring("\"a.raw\""));
    CHECK(index_json.find("\"a.manifold.json\"") < index_json.find("\"catalog/manifold-local.xml\""));
    CHECK_THAT(index_json, ContainsSubstring(checksum_file_hex(dir / "catalog.xml", ChecksumKind::Blake3)));
    CHECK_THAT(index_json, ContainsSubstring(checksum_file_hex(dir / "a.manifold.json", ChecksumKind::Sha256)));

    // Manifold accepts the bundle, but not once the image changed
    CHECK_NOTHROW(verify_bundle(bundle));
    replace_entry(bundle, "a.raw", "not really an imagf");
    CHECK_THROWS_AS(verify_bundle(bundle), BuildError);
}

TEST_CASE("bundles are signed with SSH keys", "[bundle]")
{
    if (!find_tool("ssh-keygen"))
        SKIP("ssh-keygen is not installed");
    TempDir dir("sign");
    auto key = dir / "key";
    run({.argv = {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", key.string()}});
    g_autoptr(MfSigner) signer = select_signer(key.c_str());

    auto bundle = make_test_bundle(dir.path());
    sign_bundle(bundle, signer);
    CHECK_NOTHROW(verify_bundle(bundle));
    auto signature = zip_entry(bundle, "index.json.sig");
    CHECK(signature.starts_with("-----BEGIN SSH SIGNATURE-----"));

    // ssh-keygen can verify the signature with the key embedded in it
    write_file(dir / "index.json", zip_entry(bundle, "index.json"));
    write_file(dir / "index.json.sig", signature);
    auto verify = run_status({
        .argv = {"sh", "-c", "ssh-keygen -Y check-novalidate -n manifold.bundle -s index.json.sig < index.json"},
        .cwd = dir.path(),
    });
    CHECK(verify == 0);

    // a changed file is noticed before signing
    replace_entry(bundle, "catalog/manifold-local.xml", "x");
    CHECK_THROWS_AS(sign_bundle(bundle, signer), BuildError);
}

TEST_CASE("a bundle is signed if there is a default key", "[bundle]")
{
    if (!find_tool("ssh-keygen"))
        SKIP("ssh-keygen is not installed");
    // the test run points XDG_CONFIG_HOME at an empty directory
    CHECK(select_signer(nullptr) == nullptr);

    g_setenv(MF_SIGNING_KEY_ENV, "a key", TRUE);
    ScopeGuard unset_key([] {
        g_unsetenv(MF_SIGNING_KEY_ENV);
    });
    g_autoptr(MfSigner) signer = select_signer(nullptr);
    REQUIRE(signer != nullptr);
    CHECK(std::string_view(mf_signer_get_name(signer)) == "$" MF_SIGNING_KEY_ENV);
}

TEST_CASE("a bundle that cannot be signed is written unsigned", "[bundle]")
{
    if (!find_tool("ssh-keygen"))
        SKIP("ssh-keygen is not installed");
    TempDir dir("sign-fails");
    write_file(dir / "key", "not a key\n");
    g_autoptr(MfSigner) signer = select_signer((dir / "key").c_str());
    CHECK_THROWS_AS(select_signer((dir / "missing").c_str()), BuildError);
    CHECK_THROWS_AS(select_signer("no-such-key"), BuildError);

    auto bundle = make_test_bundle(dir.path());
    CHECK_THROWS_WITH(sign_bundle(bundle, signer), ContainsSubstring("was written unsigned"));
    CHECK_NOTHROW(verify_bundle(bundle));
    CHECK(zip_entry(bundle, "index.json.sig").empty());
}

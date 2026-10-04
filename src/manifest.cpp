/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "manifest.hpp"

#include "error.hpp"

#include <algorithm>
#include <glaze/glaze.hpp>
#include <manifold.h>

template<>
struct glz::meta<mfb::BaseRef> {
    using T = mfb::BaseRef;
    static constexpr auto value = object(
        "component",
        &T::component,
        "version",
        &T::version,
        "revision",
        &T::revision,
        "sdk",
        &T::sdk,
        "identity",
        &T::identity,
        "content-hash",
        &T::content_hash);
};

template<>
struct glz::meta<mfb::BuildInfo> {
    using T = mfb::BuildInfo;
    static constexpr auto value = object(
        "timestamp",
        &T::timestamp,
        "source-date-epoch",
        &T::source_date_epoch,
        "distribution",
        &T::distribution,
        "suite",
        &T::suite,
        "snapshot-url",
        &T::snapshot_url,
        "recipe-hash",
        &T::recipe_hash,
        "builder",
        &T::builder,
        "tools",
        &T::tools);
};

template<>
struct glz::meta<mfb::AppInfo> {
    using T = mfb::AppInfo;
    static constexpr auto value =
        object("command", &T::command, "kit-strategy", &T::kit_strategy, "run-on-sdk", &T::run_on_sdk);
};

template<>
struct glz::meta<mfb::SourceRecord> {
    using T = mfb::SourceRecord;
    static constexpr auto value = object(
        "part",
        &T::part,
        "type",
        &T::type,
        "url",
        &T::url,
        "path",
        &T::path,
        "commit",
        &T::commit,
        "sha256",
        &T::sha256,
        "tree-hash",
        &T::tree_hash);
};

template<>
struct glz::meta<mfb::ImageFile> {
    using T = mfb::ImageFile;
    static constexpr auto value = object(
        "file",
        &T::file,
        "size",
        &T::size,
        "blake3",
        &T::blake3,
        "sha256",
        &T::sha256,
        "verity-root-hash",
        &T::verity_root_hash,
        "data-offset",
        &T::data_offset,
        "data-size",
        &T::data_size,
        "hash-offset",
        &T::hash_offset,
        "hash-size",
        &T::hash_size,
        "signature-offset",
        &T::signature_offset,
        "signature-size",
        &T::signature_size,
        "erofs",
        &T::erofs);
};

template<>
struct glz::meta<mfb::Manifest> {
    using T = mfb::Manifest;
    static constexpr auto value = object(
        "format",
        &T::format,
        "component",
        &T::component,
        "kind",
        &T::kind,
        "version",
        &T::version,
        "revision",
        &T::revision,
        "architecture",
        &T::architecture,
        "sdk",
        &T::sdk,
        "identity",
        &T::identity,
        "content-hash",
        &T::content_hash,
        "base",
        &T::base,
        "build",
        &T::build,
        "packages",
        &T::packages,
        "app",
        &T::app,
        "sources",
        &T::sources,
        "image",
        &T::image);
};

namespace mfb
{

namespace
{

/**
 * Check that every hash of the manifest is lowercase hex of 256 bits, as
 * Manifold requires.
 */
void check_hashes(const Manifest &manifest, const fs::path &path)
{
    auto check = [&](std::string_view key, const std::string &hash) {
        if (!mf_hex_hash_is_valid(hash.c_str()))
            fail("{}: '{}' is not a valid hash: '{}'", path.string(), key, hash);
    };
    check("identity", manifest.identity);
    check("content-hash", manifest.content_hash);
    if (manifest.base) {
        check("base.identity", manifest.base->identity);
        check("base.content-hash", manifest.base->content_hash);
    }
    if (manifest.image) {
        check("image.blake3", manifest.image->blake3);
        check("image.sha256", manifest.image->sha256);
        check("image.verity-root-hash", manifest.image->verity_root_hash);
    }
}

} // namespace

std::string arch_name_for_filename(std::string_view architecture)
{
    std::string name(architecture);
    std::ranges::replace(name, '_', '-');
    return name;
}

std::string to_json(const Manifest &manifest)
{
    auto json = glz::write<glz::opts{.prettify = true}>(manifest);
    if (!json)
        fail("Cannot serialise manifest: {}", glz::format_error(json.error()));
    return *json + "\n";
}

Manifest manifest_from_json(std::string_view json, std::string_view origin)
{
    Manifest manifest;
    // tolerate unknown keys, the manifest may come from a newer builder
    auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(manifest, json);
    if (error)
        fail("{}: {}", origin, glz::format_error(error, json));
    return manifest;
}

void write_manifest(const Manifest &manifest, const fs::path &path)
{
    check_hashes(manifest, path);
    write_file(path, to_json(manifest));
}

} // namespace mfb

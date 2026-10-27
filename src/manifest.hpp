/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "utils.hpp"

namespace mfb
{

/**
 * Packages installed in a tree, as name -> version sorted by name.
 * Used for every package system.
 */
using InstalledPackages = std::map<std::string, std::string>;

/**
 * Reference to the image an image is layered on.
 */
struct BaseRef {
    std::string component;
    std::string version;
    int64_t revision = 0;
    bool sdk = false;
    std::string identity;
    std::string content_hash;
};

struct BuildInfo {
    /**
     * Build time in ISO 8601 (UTC). We only write it to the bundle manifest file,
     * because inside the image it would make identical builds differ.
     */
    std::optional<std::string> timestamp;
    int64_t source_date_epoch = 0; /// SOURCE_DATE_EPOCH of the build, the archive snapshot time for cores

    std::optional<std::string> distribution; /// Only set for cores
    std::optional<std::string> suite;
    std::optional<std::string> snapshot_url;

    std::optional<std::string> recipe_hash; /// Recipe::hash, apps only (part of their content hash)
    std::string builder;
    std::map<std::string, std::string> tools;
};

struct ErofsInfo {
    std::string compression;
    std::string uuid;
};

/**
 * Information about the finished image file.
 */
struct ImageFile {
    std::string file;
    uint64_t size = 0;
    std::string blake3;
    std::string sha256;
    std::string verity_root_hash;
    uint64_t data_offset = 0;
    uint64_t data_size = 0;
    uint64_t hash_offset = 0;
    uint64_t hash_size = 0;
    uint64_t signature_offset = 0;
    uint64_t signature_size = 0;
    ErofsInfo erofs;
};

/**
 * How an application is run.
 */
struct AppInfo {
    std::string command;
    std::string kit_strategy;
    bool run_on_sdk = false;
};

/**
 * The exact origin of a part's source: the commit of a git source, the sha256
 * of a file, archive or patch, or the tree_hash() of a directory.
 */
struct SourceRecord {
    std::string part;
    std::string type;
    std::optional<std::string> url;
    std::optional<std::string> path; /// A local file or directory, as the recipe names it
    std::optional<std::string> commit;
    std::optional<std::string> sha256;    /// Checksum of a file, archive or patch
    std::optional<std::string> tree_hash; /// tree_hash() of a directory

    /**
     * Get the commit or checksum that pins the source.
     */
    std::string pin() const
    {
        return commit.value_or(sha256.value_or(tree_hash.value_or("")));
    }
};

/**
 * The manifest of an image. It is stored as manifold.json inside the image,
 * and (with the `image` section added) in a bundle file as well.
 */
struct Manifest {
    int format = 1;
    std::string component;
    std::string kind;     /// "core", "platform", "kit", "app" or "plugin"
    std::string version;  /// Version from the recipe. A kit takes the version of its base.
    int64_t revision = 0; /// Revision of this version: Set either by the repo, or at build-time
    std::string architecture;
    bool sdk = false;

    /**
     * blake3 over base, architecture and package names. Package upgrades
     * keep it, added or removed packages change it. An application's
     * identity is its content hash, because no other build can replace it.
     */
    std::string identity;
    /**
     * blake3 over the same, plus component, version and package versions.
     * Unique to this exact build.
     */
    std::string content_hash;

    std::optional<BaseRef> base;
    BuildInfo build;
    std::map<std::string, InstalledPackages> packages; /// By package system ("apt"): name -> version.
    std::optional<AppInfo> app;
    std::optional<std::vector<SourceRecord>> sources;
    std::optional<ImageFile> image;

    BaseRef as_base() const
    {
        return {component, version, revision, sdk, identity, content_hash};
    }
};

/**
 * Get the architecture as used in file names, with "-" instead of "_"
 * (x86-64). We use "_" to separate the parts of a file name.
 */
std::string arch_name_for_filename(std::string_view architecture);

/**
 * Get the file name of an image, or of a file that belongs to it:
 * "<component>_<version>-<revision>_<architecture><extension>".
 * Unlike the bundle name, it includes the revision.
 */
std::string image_file_name(
    std::string_view component,
    std::string_view version,
    int64_t revision,
    std::string_view architecture,
    std::string_view extension);

/**
 * Get the file name of the image a manifest describes, or of a file that
 * belongs to it.
 */
std::string image_file_name(const Manifest &manifest, std::string_view extension);

/**
 * Get the file name of a bundle: "<component>_<version>_<architecture>.manifold".
 */
std::string bundle_file_name(std::string_view component, std::string_view version, std::string_view architecture);

std::string to_json(const Manifest &manifest);
Manifest manifest_from_json(std::string_view json, std::string_view origin);

/**
 * Write a manifest to `path`. Fails the build if Manifold would reject one
 * of its hashes.
 */
void write_manifest(const Manifest &manifest, const fs::path &path);

} // namespace mfb

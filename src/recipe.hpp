/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "utils.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mfb
{

/**
 * Paths below /usr that always go to the SDK, even if the recipe does not
 * list them. We never ship documentation in a runtime.
 */
inline const std::vector<std::string> ALWAYS_SDK_ONLY_DIRS = {"share/doc"};

/**
 * Package names by package system, e.g. `apt: [...]`. A build only accepts
 * the package system of its core.
 */
using PackageSet = std::map<std::string, std::vector<std::string>>;

/**
 * Settings for creating the base tree of a core. The CoreSystem of the
 * distribution decides which keys apply and what they mean.
 */
struct Bootstrap {
    std::string distribution = "debian";
    std::string suite;
    std::string architecture; /// Architecture in the dustro naming scheme, host arch if empty
    std::string snapshot;     /// Timestamp of the archive state, anything date(1) accepts. Also the SOURCE_DATE_EPOCH
    std::vector<std::string> exclude; /// Packages to leave out of the bootstrap.
    bool slim = true; /// If true, drop documentation, manual pages and message catalogs (except for selected locale)
};

struct ImageSettings {
    std::string compression = "zstd";
    int compression_level = 12;
};

/**
 * The image an action or file belongs to: the runtime, or the SDK layer on top.
 */
enum class Stage {
    runtime,
    sdk,
};

/**
 * A script a core runs inside its image tree.
 */
struct Action {
    std::string name;
    Stage stage = Stage::runtime;
    std::string run;
};

/**
 * A source of a part. The fetchers in sources/ define which keys apply to
 * which type.
 */
struct Source {
    std::string type;
    std::string dest; /// Where the source goes, relative to the part's source directory.
    std::string url;
    std::string path; /// A local file or directory, relative to the file the part is written in.
    std::string tag;
    std::string branch;
    std::string commit;
    std::string sha256; /// Checksum of an archive, file or patch, required if it is downloaded.
    int strip_components =
        1; /// Number of leading path components to strip from archive members / from the file names in a patch.
    std::string dest_filename; /// Name to store a file source under (its own name by default)
};

/**
 * The `build-options` of a recipe or a part
 */
struct BuildSettings {
    std::string cflags;
    std::string cxxflags;
    std::string cppflags;
    std::string ldflags;
    std::map<std::string, std::string> env;
    std::string prepend_path;
    std::string append_path;
    std::string prepend_ld_library_path;
    std::string append_ld_library_path;
    std::string prepend_pkg_config_path;
    std::vector<std::string> config_opts;
    /**
     * Settings for a single architecture, keyed by its Manifold name
     * (x86_64, aarch64). They apply after the general ones.
     */
    std::map<std::string, BuildSettings> arch;
};

struct Part;

/**
 * An entry of `parts`: a part, or the path to a YAML file that contains one
 * (relative to the file the entry is in). Loading the recipe replaces each
 * path with the part it names.
 */
using PartEntry = std::variant<std::string, Part>;

/**
 * A piece of software built from source that is part of the final image.
 */
struct Part {
    std::string name;
    std::string buildsystem; /// One of meson, cmake, cmake-make, autotools or simple
    std::string subdir;      /// Subdirectory of the sources to build, instead of the top directory
    std::vector<std::string> config_opts;
    std::vector<std::string> make_args;
    std::vector<std::string> make_install_args;
    std::vector<std::string>
        build_commands; /// Shell commands to run after the build system built the part and before it installs it
    std::vector<std::string> post_install; /// Shell commands to run after the part is installed

    /**
     * Files installed by this part to remove once all parts are built.
     * A pattern starting with "/" is a path below the prefix, any other
     * pattern matches a file or directory name anywhere below it.
     */
    std::vector<std::string> cleanup;

    std::vector<std::string> only_arches;
    std::vector<std::string> skip_arches;
    std::optional<BuildSettings> build_options;
    std::vector<Source> sources;
    std::vector<PartEntry> parts; /// Parts built before this one, in this order

    /**
     * Directory of the file that defines this part. Local sources are
     * relative to it. Not a recipe key.
     */
    fs::path dir;

    /**
     * Check if only-arches and skip-arches allow building the part on an
     * architecture (by its Manifold name).
     */
    bool builds_on(std::string_view architecture) const;
};

struct Recipe {
    std::string type = "app"; /// "core", "platform" or "app"
    std::string component;
    std::string version;

    /**
     * MetaInfo file of the main image, relative to the recipe. It replaces
     * the one the image's tree ships.
     */
    std::optional<std::string> metainfo_file;

    // for the generated AppStream MetaInfo of cores and platforms
    std::string name;
    std::string summary;
    std::string description; /// Plain text, with blank lines between paragraphs
    std::string project_license;
    std::string metadata_license = "FSFAP";
    std::string homepage;

    // core
    std::optional<Bootstrap> bootstrap;
    std::vector<Action> actions;

    // app and platform
    std::string base; /// The layer below: a core or platform for apps, a core for platforms

    // app
    std::string command;      /// Program in /app/bin or the PATH to run, by its name
    std::string base_version; /// Oldest base version an app accepts. Platforms are built on the current core.
    std::string kit_strategy = "roaming"; /// How the application relates to its kit: "roaming" or "strict"
    bool generate_sdk = false;
    bool run_on_sdk = false;

    // common
    std::optional<BuildSettings>
        build_options; /// Build options for every part, a part's own build-options apply on top.
    std::vector<PartEntry> parts;
    PackageSet packages;
    PackageSet packages_sdk;

    /**
     * Patterns that mark development packages. A package listed under
     * `packages` that matches goes to the SDK, and its dependencies that do
     * not match go to the runtime. The default depends on the distribution.
     */
    std::optional<std::vector<std::string>> sdk_patterns;

    /**
     * Locales a core generates as UTF-8 and keeps message catalogs for,
     * e.g. `de_DE`. Without any, the image only has C.UTF-8.
     */
    std::vector<std::string> locales;

    ImageSettings image;

    /**
     * Shell globs of paths below the prefix (/usr for cores and platforms,
     * /app for apps) that only the SDK keeps. We move them out of the
     * runtime or app image.
     */
    std::vector<std::string> sdk_only;

    /**
     * blake3 of the parsed recipe and its included parts, serialised as JSON.
     * Formatting and comments in the YAML files do not change it. Not a
     * recipe key. Part of an application's content hash.
     */
    std::string hash;

    bool is_core() const
    {
        return type == "core";
    }
    bool is_platform() const
    {
        return type == "platform";
    }
};

/**
 * Read and validate a recipe, together with the part files it includes.
 * Any problem fails the build.
 */
Recipe load_recipe(const fs::path &path);

/**
 * Parse a recipe from YAML text. `origin` names the recipe in error
 * messages, and included part files are looked up relative to `dir`.
 */
Recipe parse_recipe(std::string_view yaml, std::string_view origin, const fs::path &dir = fs::current_path());

/**
 * Get the parts in build order, each one after the parts nested in it.
 * A part that is not built on `architecture` is skipped with its nested parts.
 */
std::vector<const Part *> parts_to_build(const Recipe &recipe, std::string_view architecture);

/**
 * Combine the build-options for a part on `architecture`.
 *
 * We apply the options of the recipe, the recipe's architecture options, the
 * part's options and the part's architecture options in this order. Compiler
 * and linker flags are joined and config-opts appended, while for environment
 * variables and paths the last value wins.
 */
BuildSettings merge_build_settings(const Recipe &recipe, const Part &part, std::string_view architecture);

} // namespace mfb

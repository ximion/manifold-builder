/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "manifest.hpp"
#include "recipe.hpp"
#include "sandbox.hpp"
#include "utils.hpp"

namespace mfb
{

class CoreSystem;
struct InstallTarget;
struct MetaInfo;

struct BuildOptions {
    std::optional<fs::path> output;   ///< The bundle filename to write
    std::optional<fs::path> sign_key; ///< SSH key to sign the bundle's index with
    std::optional<fs::path> work_dir; ///< Scratch directory. Defaults to _build-<recipe name> beside the recipe
    bool keep_work = false;
    std::optional<fs::path> cache_dir; ///< Where downloads, git mirrors and packages are kept for later builds
    unsigned jobs = 0;                 ///< Parallel jobs of a part build, one per CPU if 0

    /**
     * Location of the layers that app and platform builds are based on.
     * This is either a Manifold store (the host's by default) or a plain
     * directory of extracted bundles.
     */
    fs::path runtimes_dir = "/var/lib/manifold";
};

/**
 * One of the images a build produces.
 */
struct Output {
    std::string stem; ///< Name stem of its files in the work directory (runtime, sdk, kit, kit-sdk, app or app-sdk)
    fs::path tree;    ///< Tree the image is made from
    Manifest manifest;
};

/**
 * Base class of a build, from the recipe to the bundle.
 *
 * We drive the build with shell scripts that run as root in a user namespace,
 * and read their results in between. Package handling is up to the CoreSystem
 * of the core's distribution.
 * Subclasses set up the build in prepare() and build the trees in stages(),
 * registering an Output for each image. This class adds the manifests and
 * MetaInfo to the trees, makes the images and the bundle, and manages the
 * work directory.
 */
class Build
{
public:
    virtual ~Build();

    /**
     * Run the build and return the path of the bundle.
     */
    fs::path run();

protected:
    Build(const Recipe &recipe, const fs::path &recipe_file, const BuildOptions &options);

    /**
     * Set up the CoreSystem and find the layers to build on, if any.
     */
    virtual void prepare() = 0;

    /**
     * Build the trees and register an Output for each image.
     */
    virtual void stages() = 0;

    /**
     * Generate the MetaInfo data for a runtime image from its manifest.
     */
    virtual MetaInfo metainfo(const Manifest &manifest) const;

    /**
     * Add the install of `packages` into `tree` as a step of the script.
     *
     * The build's package cache is bound into the tree meanwhile, so no
     * install of the build downloads a package again.
     */
    void install_packages(Script &s, const fs::path &tree, const std::vector<std::string> &packages, std::string step)
        const;

    /**
     * Add a step that dumps the dependencies of the recipe's development
     * packages, as `tree` resolves them, if it has any. runtime_packages()
     * reads the dump.
     */
    void dump_development_dependencies(Script &s, const fs::path &tree) const;

    /**
     * Get the packages a runtime needs: the recipe's own and those the
     * recipe's development packages depend on.
     */
    std::vector<std::string> runtime_packages() const;

    /**
     * Run the recipe's actions for one stage.
     * `tree` is the image tree as seen from inside the namespace.
     */
    void run_actions(Script &s, Stage stage, const fs::path &tree) const;

    /**
     * Fetch the sources of all parts we build.
     */
    void fetch_sources();

    /**
     * Add the build of every part to the script, installing into `prefix_dir`.
     *
     * Each part is built in a nested bubblewrap sandbox over `root`, with its
     * sources, build options and the MANIFOLD_* variables. We bind
     * `prefix_dir` at the install prefix, parts install into it directly and
     * run their post-install commands there, and we record the files each part
     * adds for files_added().
     */
    void build_parts(Script &s, const fs::path &root, const InstallTarget &target, const fs::path &prefix_dir) const;

    /**
     * Add the build of every part to the script, staging what it installs
     * below /usr.
     *
     * Each part installs into its part_stage() with DESTDIR. It is built on a
     * read-only overlay of the stages of the parts before it over `lowers`
     * (top-most first), so it sees what they installed and can write nothing
     * else. The caller applies cleanup_stages() and install_stages() later.
     */
    void stage_parts(Script &s, const std::vector<fs::path> &lowers) const;

    /**
     * Get the staging directory a part installs into with DESTDIR.
     */
    fs::path part_stage(const Part &part) const;

    /**
     * Apply the cleanup patterns of every part to its part_stage().
     */
    void cleanup_stages() const;

    /**
     * Copy every part_stage() into `tree`, and run the part's post-install
     * commands there.
     */
    void install_stages(Script &s, const fs::path &tree) const;

    /**
     * List the files a part added below the prefix directory of build_parts().
     */
    std::vector<std::string> files_added(const Part &part) const;

    /**
     * Remove the `files` that match the part's cleanup patterns, and any
     * directories left empty. `files` are relative to `root`, the part's prefix.
     */
    void cleanup_part(const Part &part, const fs::path &root, const std::vector<std::string> &files) const;

    /**
     * Create a manifest with the fields all manifests of this build share.
     */
    Manifest manifest_for(
        std::string component,
        std::string kind,
        std::string version,
        bool sdk,
        const InstalledPackages &packages) const;

    /**
     * Get the content hash lines for the parts' sources.
     */
    std::vector<std::string> source_lines() const;

    /**
     * Find a registered output by its stem.
     */
    const Output &output(std::string_view stem) const;

    const Recipe &m_recipe;
    fs::path m_recipe_file;
    fs::path m_recipe_dir;
    const BuildOptions &m_options;
    std::unique_ptr<CoreSystem> m_system;

    // set up by run() after prepare()
    int64_t m_epoch = 0;
    std::string m_architecture;        ///< Manifold name of the architecture, as used in manifests
    std::vector<const Part *> m_parts; ///< Parts to build on this architecture, in order
    BuildInfo m_build_info;
    fs::path m_output;
    fs::path m_work;
    fs::path m_cache;     ///< Downloads, git mirrors and packages, in the work dir without a cache
    int m_work_lock = -1; ///< Locked descriptor of m_work, so builds can not share it
    Sandbox m_sandbox;

    std::vector<SourceRecord> m_sources;
    std::vector<Output> m_outputs;

private:
    void setup();
    /**
     * Create, lock and clear the work directory.
     *
     * We only clear a directory that carries our .gitignore, and refuse to
     * use any other directory that is not empty.
     */
    void prepare_work_dir();
    fs::path dependencies_dump() const;
    std::vector<std::pair<std::string, std::string>> part_environment(
        const BuildSettings &settings,
        const InstallTarget &target) const;
    /**
     * Get the directory a part's sources are fetched into.
     */
    fs::path part_sources(const Part &part) const;
    /**
     * Get a file we keep about a part next to its stage, e.g. its build script.
     */
    fs::path part_file(const Part &part, std::string_view suffix) const;
    /**
     * Get the sandbox of a part's build over `root`, without anything to
     * install into.
     */
    PartBuild part_build(const Part &part, const fs::path &root, const InstallTarget &target) const;

    /**
     * Add the manifest and MetaInfo to every output tree, write the catalog
     * of the bundle, and pack the trees.
     */
    void pack_outputs();
    /**
     * Add the commands that put an output's MetaInfo file at tree_metainfo().
     *
     * The main image takes the recipe's metainfo-file, and every image the file
     * its tree ships. We remove the other MetaInfo files of the component.
     * Returns the file we take, or an empty path if there is none.
     */
    fs::path place_metainfo(Script &s, const Output &output) const;
    /**
     * Generate the MetaInfo file of an output that ships none, and return its path.
     *
     * Runtimes get theirs from the recipe, SDKs from their runtime or app. An
     * app has to ship its own. `placed` maps the components before the output
     * to their MetaInfo files.
     */
    fs::path generate_metainfo_file(const Output &output, const std::map<std::string, fs::path> &placed) const;
    /**
     * Generate the MetaInfo data for an SDK from the MetaInfo of the runtime
     * or app `runtime_id`, which it extends.
     */
    MetaInfo sdk_metainfo(const Manifest &manifest, const std::string &runtime_id, const MetaInfo &runtime) const;
    /**
     * Get the path of an output's MetaInfo file in its tree,
     * "<prefix>/share/metainfo/<component>.metainfo.xml".
     */
    fs::path tree_metainfo(const Output &output) const;
    /**
     * Install an output's manifest into its tree. App images keep it below /app.
     */
    void install_manifest(Script &s, const Output &output) const;
    /**
     * Normalize an output's tree and pack it into an erofs image.
     * Everything in the image is owned by root, without setuid or setgid bits.
     */
    void pack(Script &s, const Output &output) const;
    fs::path make_images();
    void cleanup_work_dir();
};

} // namespace mfb

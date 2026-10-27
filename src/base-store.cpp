/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "base-store.hpp"

#include "appstream.hpp"
#include "error.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "utils.hpp"

#include <glib.h>
#include <glib/gstdio.h>
#include <manifold.h>
#include <unistd.h>

#include <optional>

namespace mfb
{

namespace
{

quill::Logger *const logStore = log::logger("store");

/**
 * Check if a version is at least `minimum`, or if there is no minimum.
 */
bool acceptable(const std::string &version, const std::string &minimum)
{
    return minimum.empty() || mf_version_compare(version.c_str(), minimum.c_str()) >= 0;
}

MfComponentKind kind_of(const Manifest &manifest)
{
    return mf_component_kind_from_string(manifest.kind.c_str());
}

std::optional<Manifest> read_manifest(const fs::path &path)
{
    // skip anything Manifold would not accept
    g_autoptr(GError) error = nullptr;
    g_autoptr(MfManifest) checked = mf_manifest_new_from_file(path.c_str(), &error);
    if (checked == nullptr) {
        LOG_INFO(logStore, "Ignoring {}: {}", path.string(), error->message);
        return std::nullopt;
    }
    try {
        return manifest_from_json(read_file(path), path.string());
    } catch (const BuildError &error) {
        LOG_INFO(logStore, "Ignoring {}: {}", path.string(), error.what());
        return std::nullopt;
    }
}

bool is_layer(const Manifest &manifest, const std::string &component, bool sdk)
{
    auto kind = kind_of(manifest);
    return (kind == MF_COMPONENT_KIND_CORE || kind == MF_COMPONENT_KIND_PLATFORM) && manifest.sdk == sdk
           && manifest.component == component && manifest.image;
}

/**
 * Check if `store` holds any components. We treat a directory without
 * components as a plain directory of extracted bundles.
 */
bool is_store(MfStore *store)
{
    g_autoptr(GError) error = nullptr;
    g_autoptr(GPtrArray) components = mf_store_list_components(store, nullptr, MF_COMPONENT_KIND_UNKNOWN, &error);
    if (components == nullptr)
        fail("Cannot read the store at {}: {}", mf_store_get_path(store), error->message);
    return components->len > 0;
}

MfComponent *get_component(MfStore *store, const std::string &component, const char *version)
{
    g_autoptr(GError) error = nullptr;
    MfComponent *found = mf_store_get_component(store, component.c_str(), version, &error);
    if (found == nullptr)
        fail("{}", error->message);
    return found;
}

/**
 * Read the manifest of an installed component and get the path of its image.
 */
Manifest read_component(MfStore *store, MfComponent *component, fs::path &image_path)
{
    const char *id = mf_component_get_id(component);
    const char *version = mf_component_get_version(component);

    g_autoptr(GError) error = nullptr;
    g_autoptr(MfManifest) manifest = mf_component_fetch_manifest(component, &error);
    if (manifest == nullptr)
        fail("{}", error->message);

    g_autofree gchar *image = mf_store_get_image_path(store, component);
    image_path = image;
    if (!fs::is_regular_file(image_path))
        fail("The image of {} {} is missing", id, version);

    gsize size = 0;
    auto *data = static_cast<const char *>(g_bytes_get_data(mf_manifest_get_data(manifest), &size));
    return manifest_from_json(std::string_view(data, size), std::format("Manifest of {} {}", id, version));
}

/**
 * Read an installed layer from a Manifold store: `runtime` and the SDK of
 * the same version.
 */
LayerImages read_installed(MfStore *store, MfComponent *runtime)
{
    std::string component = mf_component_get_id(runtime);
    LayerImages layer;
    layer.runtime = read_component(store, runtime, layer.runtime_image);
    if (!is_layer(layer.runtime, component, false))
        fail("{} is not a core or a platform", component);

    auto sdk_component = component + ".sdk";
    g_autoptr(MfComponent) sdk = get_component(store, sdk_component, layer.runtime.version.c_str());
    layer.sdk = read_component(store, sdk, layer.sdk_image);
    if (!is_layer(layer.sdk, sdk_component, true))
        fail("{} is not the SDK of {}", sdk_component, component);
    if (!layer.sdk.base || layer.sdk.base->content_hash != layer.runtime.content_hash)
        fail("The installed SDK of {} is layered on another image than the version in use", component);
    return layer;
}

/**
 * Find a layer in a plain directory of extracted bundles, as the builder
 * writes them.
 *
 * We take the build with `content_hash` if one is given, and the newest one
 * of at least `minimum_version` otherwise.
 */
LayerImages find_bundled(
    const fs::path &dir,
    const std::string &component,
    const std::string &minimum_version,
    const std::string &content_hash = {})
{
    std::optional<Manifest> best;
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (!entry.path().filename().string().ends_with(".manifold.json"))
            continue;
        auto manifest = read_manifest(entry.path());
        if (!manifest || !is_layer(*manifest, component, false))
            continue;
        if (content_hash.empty() ? !acceptable(manifest->version, minimum_version)
                                 : manifest->content_hash != content_hash)
            continue;
        if (!best
            || mf_version_compare_with_rev(
                   manifest->version.c_str(),
                   static_cast<guint64>(manifest->revision),
                   best->version.c_str(),
                   static_cast<guint64>(best->revision))
                   > 0)
            best = manifest;
    }
    if (!best && !content_hash.empty())
        fail("No build of {} with content hash {} found in {}", component, content_hash, dir.string());
    if (!best)
        fail(
            "No {} version {} found in {}",
            component,
            minimum_version.empty() ? "at all" : minimum_version,
            dir.string());

    LayerImages layer;
    layer.runtime = *best;
    layer.runtime_image = dir / best->image->file;
    auto sdk_manifest = dir
                        / image_file_name(
                            component + ".sdk", best->version, best->revision, best->architecture, ".manifold.json");
    auto sdk = read_manifest(sdk_manifest);
    if (!sdk || !is_layer(*sdk, component + ".sdk", true))
        fail("{} {} has no SDK manifest at {}", component, best->version, sdk_manifest.string());
    layer.sdk = *sdk;
    layer.sdk_image = dir / sdk->image->file;
    for (const auto &image : {layer.runtime_image, layer.sdk_image}) {
        if (!fs::is_regular_file(image))
            fail("Image {} is missing", image.string());
    }
    return layer;
}

/**
 * Read the MetaInfo of the base from its image, to get the license and
 * homepage for the kits built on it. Without it, the kits name neither.
 */
void read_base_metainfo(Stack &stack)
{
    const auto &base = stack.base();
    g_autoptr(GError) error = nullptr;
    g_autofree gchar *tmp_name = nullptr;
    int fd = g_file_open_tmp("manifold-builder-XXXXXX.metainfo.xml", &tmp_name, &error);
    if (fd < 0)
        fail("Cannot create a temporary file: {}", error->message);
    close(fd);
    fs::path metainfo = tmp_name;

    Command extract;
    extract.argv = {
        require_tool("fsck.erofs").string(),
        "--offset=" + std::to_string(base.runtime.image->data_offset),
        "--extract=" + metainfo.string(),
        "--overwrite",
        "--no-preserve",
        "--path=/usr/share/metainfo/" + base.runtime.component + ".metainfo.xml",
        base.runtime_image.string(),
    };
    extract.quiet = !log::verbose();
    if (run_status(extract) != 0) {
        LOG_WARNING(
            logStore, "{} carries no MetaInfo - its kits will name no license or homepage", base.runtime.component);
        fs::remove(metainfo);
        return;
    }

    try {
        auto info = parse_metainfo(read_file(metainfo));
        stack.project_license = info.project_license;
        stack.homepage = info.homepage;
    } catch (const BuildError &e) {
        LOG_WARNING(logStore, "Can't read the MetaInfo of {}: {}", base.runtime.component, e.what());
    }
    fs::remove(metainfo);
}

} // namespace

Stack find_stack(const fs::path &dir, const std::string &base, const std::string &minimum_version)
{
    if (!fs::is_directory(dir))
        fail("Runtimes directory {} does not exist", dir.string());
    g_autoptr(MfStore) store = mf_store_new_for_path(dir.c_str());
    bool installed = is_store(store);

    Stack stack;
    g_autoptr(MfComponent) runtime = nullptr;
    LayerImages layer;
    if (installed) {
        // take the current version from the store, if it is new enough
        runtime = get_component(store, base, nullptr);
        layer = read_installed(store, runtime);
        if (!acceptable(layer.runtime.version, minimum_version))
            fail(
                "The installed {} is version {}, but {} or newer is needed",
                base,
                layer.runtime.version,
                minimum_version);
    } else {
        layer = find_bundled(dir, base, minimum_version);
    }

    if (kind_of(layer.runtime) == MF_COMPONENT_KIND_PLATFORM) {
        // a platform requires the exact core it was built on
        if (!layer.runtime.base)
            fail("Platform {} names no core", base);
        if (installed) {
            // the store links a platform to its core
            g_autoptr(GError) error = nullptr;
            g_autoptr(MfComponent) core = mf_store_get_base(store, runtime, &error);
            if (core == nullptr)
                fail("{}", error->message);
            stack.core = read_installed(store, core);
        } else {
            // a rebuild of the same core version is a different core
            stack.core = find_bundled(dir, layer.runtime.base->component, {}, layer.runtime.base->content_hash);
        }
        if (stack.core.runtime.content_hash != layer.runtime.base->content_hash)
            fail(
                "{} {} in {} is not the core platform {} was built on",
                stack.core.runtime.component,
                stack.core.runtime.version,
                dir.string(),
                base);
        stack.platform = std::move(layer);
    } else {
        stack.core = std::move(layer);
    }
    read_base_metainfo(stack);
    LOG_INFO(logStore, "Using {} {} from {}", base, stack.base().runtime.version, dir.string());
    if (stack.platform)
        LOG_INFO(logStore, "The platform core is {} {}", stack.core.runtime.component, stack.core.runtime.version);
    return stack;
}

} // namespace mfb

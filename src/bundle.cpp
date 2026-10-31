/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "bundle.hpp"

#include "error.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "utils.hpp"

namespace mfb
{

quill::Logger *const logBundle = log::logger("bundle");

void write_bundle(
    const fs::path &bundle_path,
    const std::vector<BundleImage> &images,
    const fs::path &icons_dir,
    const fs::path &media_dir,
    MfSigner *signer)
{
    LOG_INFO(logBundle, "Writing {}...", bundle_path.filename().string());

    g_autoptr(GError) error = nullptr;
    g_autoptr(MfBundleWriter) writer = mf_bundle_writer_new(bundle_path.c_str(), &error);
    if (writer == nullptr)
        fail("{}", error->message);

    for (const auto &image : images) {
        g_autoptr(GBytes) manifest = g_bytes_new(image.manifest.data(), image.manifest.size());
        g_autoptr(GBytes) catalog = g_bytes_new(image.catalog.data(), image.catalog.size());
        if (!mf_bundle_writer_add_image(writer, image.file.c_str(), manifest, catalog, &error))
            fail("{}", error->message);
    }
    if (!mf_bundle_writer_add_catalog_assets(writer, icons_dir.c_str(), media_dir.c_str(), &error))
        fail("{}", error->message);

    if (signer != nullptr)
        LOG_INFO(logBundle, "Signing with {}...", mf_signer_get_name(signer));
    if (mf_bundle_writer_finish(writer, signer, &error))
        return;
    if (signer == nullptr)
        fail("{}", error->message);

    // the build is not lost, the bundle can be signed on its own
    g_autoptr(GError) unsigned_error = nullptr;
    if (!mf_bundle_writer_finish(writer, nullptr, &unsigned_error))
        fail("{}", error->message);
    fail(
        "{}\n{} was written unsigned, sign it with: manifold bundle sign [--key=KEY] {}",
        error->message,
        bundle_path.string(),
        shell_quote(bundle_path.string()));
}

MfSigner *select_signer(const char *key)
{
    g_autoptr(GError) error = nullptr;
    g_autoptr(MfSigner) signer = key ? mf_signer_new_for_key(key, &error) : mf_signer_new_default(&error);

    // only a key that was asked for has to exist
    if (signer == nullptr && key == nullptr) {
        LOG_INFO(logBundle, "{}, the bundle will not be signed", error->message);
        return nullptr;
    }
    if (signer == nullptr || !mf_signer_check_key(signer, &error))
        fail("{}", error->message);

    return g_steal_pointer(&signer);
}

void verify_bundle(const fs::path &bundle_path)
{
    g_autoptr(GError) error = nullptr;
    g_autoptr(MfBundle) bundle = mf_bundle_new_from_path(bundle_path.c_str(), &error);
    if (bundle == nullptr)
        fail("{}", error->message);
    GPtrArray *images = mf_bundle_get_images(bundle);
    for (guint i = 0; i < images->len; ++i) {
        auto *image = static_cast<MfBundleImage *>(g_ptr_array_index(images, i));
        if (!mf_bundle_verify_image(bundle, image, nullptr, nullptr, &error))
            fail("{}: {}", bundle_path.string(), error->message);
    }
    LOG_INFO(
        logBundle,
        "Verified {}: {} image{}, {}",
        bundle_path.filename().string(),
        images->len,
        images->len == 1 ? "" : "s",
        mf_bundle_is_signed(bundle) ? "signed" : "unsigned");
}

} // namespace mfb

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "bundle.hpp"

#include "error.hpp"
#include "checksum.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "scopeguard.hpp"
#include "utils.hpp"

#include <glaze/glaze.hpp>
#include <manifold.h>
#include <zip.h>

template<>
struct glz::meta<mfb::IndexEntry> {
    using T = mfb::IndexEntry;
    static constexpr auto value =
        object("name", &T::name, "size", &T::size, "blake3", &T::blake3, "sha256", &T::sha256);
};

template<>
struct glz::meta<mfb::BundleIndex> {
    using T = mfb::BundleIndex;
    static constexpr auto value = object("format", &T::format, "files", &T::files);
};

namespace mfb
{

quill::Logger *const logBundle = log::logger("bundle");

namespace
{

/**
 * An open ZIP archive that is discarded unless we call close().
 */
class Archive
{
public:
    Archive(const fs::path &path, int flags)
        : m_path(path)
    {
        int error_code = 0;
        m_zip = zip_open(path.c_str(), flags, &error_code);
        if (m_zip == nullptr) {
            zip_error_t error;
            zip_error_init_with_code(&error, error_code);
            std::string message = zip_error_strerror(&error);
            zip_error_fini(&error);
            fail("Cannot open {}: {}", path.string(), message);
        }
    }

    ~Archive()
    {
        if (m_zip)
            zip_discard(m_zip);
    }

    Archive(const Archive &) = delete;
    Archive &operator=(const Archive &) = delete;

    [[noreturn]] void fail_with(std::string_view what)
    {
        std::string message = zip_strerror(m_zip);
        fail("{}: {}", what, message);
    }

    void add_file(const std::string &name, const fs::path &source, bool store)
    {
        zip_source_t *zsource = zip_source_file(m_zip, source.c_str(), 0, -1);
        if (zsource == nullptr)
            fail_with("Cannot read " + source.string());
        add(name, zsource, store);
    }

    /**
     * Add a file from memory. `data` must stay alive until close().
     */
    void add_buffer(std::string_view name, const std::string &data)
    {
        zip_source_t *zsource = zip_source_buffer(m_zip, data.data(), data.size(), 0);
        if (zsource == nullptr)
            fail_with("Cannot buffer " + std::string(name));
        add(std::string(name), zsource, false);
    }

    std::string read(std::string_view name)
    {
        std::string entry(name);
        zip_stat_t stat;
        if (zip_stat(m_zip, entry.c_str(), 0, &stat) != 0 || !(stat.valid & ZIP_STAT_SIZE))
            fail("{}: No {} in the bundle", m_path.filename().string(), entry);
        zip_file_t *file = zip_fopen(m_zip, entry.c_str(), 0);
        if (file == nullptr)
            fail_with("Cannot read " + entry);
        std::string data(stat.size, '\0');
        auto got = zip_fread(file, data.data(), data.size());
        zip_fclose(file);
        if (got < 0 || static_cast<zip_uint64_t>(got) != stat.size)
            fail("Cannot read {} from {}", entry, m_path.string());
        return data;
    }

    void close()
    {
        if (zip_close(m_zip) != 0)
            fail_with("Cannot write " + m_path.string());
        m_zip = nullptr;
    }

private:
    void add(const std::string &name, zip_source_t *zsource, bool uncompressed)
    {
        zip_int64_t index = zip_file_add(m_zip, name.c_str(), zsource, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (index < 0) {
            zip_source_free(zsource);
            fail_with("Cannot add " + name);
        }
        auto method = uncompressed ? ZIP_CM_STORE : ZIP_CM_DEFLATE;
        if (zip_set_file_compression(m_zip, static_cast<zip_uint64_t>(index), method, 0) != 0)
            fail_with("Cannot set compression for " + name);
    }

    fs::path m_path;
    zip_t *m_zip = nullptr;
};

std::string index_to_json(BundleIndex index)
{
    std::ranges::sort(index.files, {}, &IndexEntry::name);
    auto json = glz::write<glz::opts{.prettify = true}>(index);
    if (!json)
        fail("Cannot serialise index.json: {}", glz::format_error(json.error()));
    return *json + "\n";
}

/**
 * Sign the index with `ssh-keygen -Y sign` and return the SSHSig signature.
 *
 * ssh-keygen only signs files, so we briefly write the index next to the
 * bundle.
 */
std::string sign_index(const std::string &index_json, const fs::path &key, const fs::path &bundle_path)
{
    auto ssh_keygen = require_tool("ssh-keygen");
    auto index_file = bundle_path.string() + "." + std::string(INDEX_NAME);
    auto signature_file = index_file + ".sig";
    // not quiet, so a passphrase prompt reaches the terminal
    std::vector<std::string> sign = {
        ssh_keygen.string(),
        "-Y",
        "sign",
        "-f",
        fs::absolute(key).string(),
        "-n",
        std::string(SIGN_NAMESPACE),
        index_file,
    };
    ScopeGuard remove_files([&] {
        std::error_code error;
        fs::remove(index_file, error);
        fs::remove(signature_file, error);
    });

    LOG_INFO(logBundle, "Signing with {}...", key.string());
    write_file(index_file, index_json);
    run({.argv = sign});
    return read_file(signature_file);
}

} // namespace

bool is_indexed(std::string_view name)
{
    return name.ends_with(".manifold.json") || name.starts_with(MF_BUNDLE_CATALOG_DIR "/");
}

void write_bundle(
    const fs::path &bundle_path,
    const std::vector<BundleEntry> &entries,
    const std::optional<fs::path> &sign_key)
{
    LOG_INFO(logBundle, "Writing {}...", bundle_path.filename().string());

    Archive archive(bundle_path, ZIP_CREATE | ZIP_TRUNCATE);
    BundleIndex index;
    for (const auto &entry : entries) {
        archive.add_file(entry.name, entry.source, entry.uncompressed);
        if (is_indexed(entry.name)) {
            auto checksums = checksum_file(entry.source);
            index.files.push_back({
                .name = entry.name,
                .size = fs::file_size(entry.source),
                .blake3 = checksums.blake3,
                .sha256 = checksums.sha256,
            });
        }
    }

    auto index_json = index_to_json(std::move(index));
    archive.add_buffer(INDEX_NAME, index_json);
    std::string signature;
    if (sign_key) {
        try {
            signature = sign_index(index_json, *sign_key, bundle_path);
        } catch (const BuildError &error) {
            // the build is not lost, the bundle can be signed on its own
            archive.close();
            fail(
                "{}\n{} was written unsigned, sign it with: manifold-builder sign {} {}",
                error.what(),
                bundle_path.string(),
                shell_quote(sign_key->string()),
                shell_quote(bundle_path.string()));
        }
        archive.add_buffer(SIGNATURE_NAME, signature);
    }
    archive.close();
}

void check_sign_key(const fs::path &key)
{
    require_tool("ssh-keygen");
    if (!fs::is_regular_file(key))
        fail("Signing key {} does not exist", key.string());
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

void sign_bundle(const fs::path &bundle_path, const fs::path &sign_key)
{
    // never sign what Manifold refuses to open, e.g. a bundle that changed
    // after its index was written
    {
        g_autoptr(GError) error = nullptr;
        g_autoptr(MfBundle) bundle = mf_bundle_new_from_path(bundle_path.c_str(), &error);
        if (bundle == nullptr)
            fail("{}, refusing to sign", error->message);
    }

    Archive archive(bundle_path, 0);
    auto index_json = archive.read(INDEX_NAME);
    auto signature = sign_index(index_json, sign_key, bundle_path);
    archive.add_buffer(SIGNATURE_NAME, signature);
    archive.close();
}

} // namespace mfb

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "image.hpp"

#include "error.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "utils.hpp"

#include <glaze/glaze.hpp>
#include <map>

namespace mfb
{

quill::Logger *const logImage = log::logger("image");

namespace
{

// space for the signature partition of the verity root hash, which
// systemd-repart sizes the same way
constexpr uint64_t DDI_SIGNATURE_SIZE = 16 * 1024;

/**
 * A partition as systemd-repart reports it with --json=short.
 */
struct RepartPartition {
    std::string type;
    std::string uuid;
    uint64_t offset = 0;
    uint64_t raw_size = 0;
    uint64_t raw_padding = 0;
    std::optional<std::string> roothash;
};

/**
 * Write the partition definitions for systemd-repart.
 *
 * We use systemd's layout for a root filesystem DDI, with two differences.
 * repart copies the root partition from the erofs image we already built
 * instead of formatting it, so we keep our reproducible mkfs.erofs
 * invocation. And instead of a signature partition we only leave free space
 * for one: systemd-dissect rejects an unsigned image with an empty signature
 * partition, because it insists on finding a signature in it.
 *
 * Neither partition sets UUID=, so repart derives the UUID of the root
 * partition from the first 128 bits of the verity root hash and the UUID of
 * the hash partition from the last 128. This is how a DDI commits to its root
 * hash in the partition table.
 */
fs::path write_repart_definitions(const fs::path &work_dir, const fs::path &erofs)
{
    auto definitions = work_dir / "repart.d";
    fs::create_directories(definitions);

    write_file(
        definitions / "10-root.conf",
        std::format(
            R"([Partition]
Type=root
Verity=data
VerityMatchKey=root
CopyBlocks={}
)",
            fs::canonical(erofs).string()));
    write_file(
        definitions / "20-root-verity.conf",
        std::format(
            R"([Partition]
Type=root-verity
Verity=hash
VerityMatchKey=root
Minimize=best
PaddingMinBytes={0}
PaddingMaxBytes={0}
)",
            DDI_SIGNATURE_SIZE));
    return definitions;
}

/**
 * Get the value of a lowercase hexadecimal digit.
 */
int hex_value(char digit)
{
    return digit >= 'a' ? digit - 'a' + 10 : digit - '0';
}

} // namespace

std::string image_uuid(std::string_view content_hash)
{
    if (content_hash.size() < 32)
        fail("Content hash '{}' is too short to derive a UUID from", content_hash);
    // We take the first 128 bits of the hash and set the version and variant
    // of RFC 9562 to make it a well-formed UUID: version 8 for custom
    // schemes, and the variant bits 10 in place of the top two bits of
    // nibble 16.
    std::string hex(content_hash.substr(0, 32));
    hex[12] = '8';
    hex[16] = "89ab"[hex_value(hex[16]) & 3];
    return std::format(
        "{}-{}-{}-{}-{}", hex.substr(0, 8), hex.substr(8, 4), hex.substr(12, 4), hex.substr(16, 4), hex.substr(20));
}

std::vector<std::string> mkfs_erofs_command(
    const fs::path &tree,
    const fs::path &output,
    int64_t epoch,
    const ImageSettings &settings,
    const std::string &uuid,
    const std::vector<std::string> &excludes)
{
    // we need each of these flags for a reproducible image:
    //   -b4096          pin the block size (defaults to the page size)
    //   -T N --all-time set every timestamp in the image to the epoch
    //   -U              set the filesystem UUID (random otherwise)
    //   --workers=1     avoid thread scheduling nondeterminism
    //   -x-1            no extended attributes, so the overlay markers of a
    //                   build layer never end up in an image
    // --all-root        drops the users and groups of the distribution, whose ids
    //                   mean nothing on the host or in an app's sandbox.
    //
    // -C65536 is for size and speed: 64 KiB physical clusters compress much
    // better than the 4 KiB default and still decompress in tens of
    // microseconds, below any storage latency.
    std::vector<std::string> argv = {
        "mkfs.erofs",
        std::format("-z{},level={}", settings.compression, settings.compression_level),
        "-b4096",
        "-C65536",
        std::format("-T{}", epoch),
        "--all-time",
        "-U" + uuid,
        "--workers=1",
        "-x-1",
        "--all-root",
        "--quiet",
    };
    for (const auto &pattern : excludes)
        argv.push_back("--exclude-regex=" + pattern);
    argv.insert(argv.end(), {output.string(), tree.string()});
    return argv;
}

std::string repart_architecture(const std::string &architecture)
{
    static const std::map<std::string, std::string> names = {
        {"x86_64", "x86-64"},
        {"aarch64", "arm64"},
        {"armel", "arm"},
        {"armhf", "arm"},
        {"i386", "x86"},
        {"powerpc64le", "ppc64-le"},
        {"riscv64", "riscv64"},
        {"s390x", "s390x"},
    };
    auto found = names.find(architecture);
    if (found == names.end())
        fail("no systemd architecture known for architecture '{}'", architecture);
    return found->second;
}

DdiLayout make_ddi(
    const fs::path &erofs,
    const fs::path &output,
    int64_t epoch,
    const std::string &architecture,
    const std::string &seed,
    const fs::path &work_dir)
{
    // two options make the image reproducible:
    //   --offline=yes  build without loop devices, the only mode in which
    //                  repart promises byte-identical output (and which
    //                  needs no privileges)
    //   --seed         the disk UUID and the dm-verity salt derive from it,
    //                  so a fixed seed gives a fixed root hash
    auto definitions = write_repart_definitions(work_dir, erofs);

    fs::remove(output);
    LOG_INFO(logImage, "Creating {}...", output.filename().string());

    Command command;
    command.argv = {
        require_tool("systemd-repart").string(),
        "--definitions=" + definitions.string(),
        "--empty=create",
        "--size=auto",
        "--offline=yes",
        "--seed=" + seed,
        "--architecture=" + repart_architecture(architecture),
        "--dry-run=no",
        "--pretty=no",
        "--json=short",
        "--no-pager",
        output.string(),
    };
    command.env["SOURCE_DATE_EPOCH"] = std::to_string(epoch);
    // repart reports every block copy on stderr, only show that when debugging
    command.quiet = !log::verbose();
    auto json = run_capture(command);

    std::vector<RepartPartition> partitions;
    auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(partitions, json);
    if (error)
        fail("Cannot parse the output of systemd-repart: {}", glz::format_error(error, json));
    if (partitions.size() != 2 || !partitions[0].roothash || partitions[0].roothash->empty())
        fail("systemd-repart did not produce the expected verity layout");

    const auto &data = partitions[0];
    const auto &hash = partitions[1];
    LOG_INFO(logImage, "Verity root hash: {}", *data.roothash);

    return DdiLayout{
        .root_hash = *data.roothash,
        .data_offset = data.offset,
        .data_size = data.raw_size,
        .hash_offset = hash.offset,
        .hash_size = hash.raw_size,
        .signature_offset = hash.offset + hash.raw_size,
        .signature_size = hash.raw_padding,
    };
}

} // namespace mfb

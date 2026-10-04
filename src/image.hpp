/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "recipe.hpp"
#include "utils.hpp"

namespace mfb
{

/**
 * Layout of the partitions in a finished image, as DDI.
 */
struct DdiLayout {
    std::string root_hash;

    uint64_t data_offset = 0;
    uint64_t data_size = 0;

    uint64_t hash_offset = 0;
    uint64_t hash_size = 0;

    /**
     * Free space after the hash partition, reserved for a signature
     * partition that a signing step adds later.
     */
    uint64_t signature_offset = 0;
    uint64_t signature_size = 0;
};

/**
 * Derive the UUID of an image from its content hash.
 *
 * A reproduced build gets the same UUID, and any other build gets a
 * different one. We use it as the erofs filesystem UUID and as the seed from
 * which systemd-repart derives the disk UUID and the verity salt.
 */
std::string image_uuid(std::string_view content_hash);

/**
 * Get the command that turns a directory tree into a reproducible erofs image.
 *
 * `excludes` are regular expressions over paths relative to the tree. The
 * command must run inside the build namespace, where the tree is readable
 * and has the ownerships set that the image should record.
 */
std::vector<std::string> mkfs_erofs_command(
    const fs::path &tree,
    const fs::path &output,
    int64_t epoch,
    const ImageSettings &settings,
    const std::string &uuid,
    const std::vector<std::string> &excludes);

/**
 * Get the systemd name of an architecture, which selects the partition types.
 */
std::string repart_architecture(const std::string &architecture);

/**
 * Wrap an erofs image in a GPT with a verity partition.
 *
 * `architecture` is the Manifold name of the architecture.
 */
DdiLayout make_ddi(
    const fs::path &erofs,
    const fs::path &output,
    int64_t epoch,
    const std::string &architecture,
    const std::string &seed,
    const fs::path &work_dir);

} // namespace mfb

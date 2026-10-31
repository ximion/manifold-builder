/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "utils.hpp"

#include <string>
#include <vector>

struct _AsMetadata;

namespace mfb
{

/**
 * Content of a generated MetaInfo file. Runtime images are components of
 * type "runtime", and their SDKs are "addon" components extending them.
 */
struct MetaInfo {
    std::string id;
    std::string kind;    ///< "runtime" or "addon"
    std::string extends; ///< The component-ID of a component an addon extends
    std::string name;
    std::string summary;
    std::string description; ///< Plain text, with blank lines between paragraphs
    std::string project_license;
    std::string metadata_license;
    std::string homepage;
    std::string version;
    std::string release_date; ///< ISO 8601 date of the latest release
};

/**
 * Generate MetaInfo XML for an image.
 */
std::string generate_metainfo(const MetaInfo &info);

/**
 * A problem the AppStream validator found in MetaInfo XML.
 */
struct MetaInfoIssue {
    bool error; ///< An error, not only a warning
    std::string text;
};

/**
 * Validate MetaInfo XML.
 * Returns its errors and warnings, nothing if the data is valid.
 */
std::vector<MetaInfoIssue> validate_metainfo(const std::string &xml);

/**
 * Read the data of a component from MetaInfo XML.
 * We leave out a bunch of stuff and parse only what the build process needs.
 */
MetaInfo parse_metainfo(const std::string &xml);

/**
 * Run appstream-compose on an image tree with the given prefix, and return
 * catalog XML with the component `component_id`. Its icons and screenshots go
 * to `icons_dir` and `media_dir`. All other components of the tree that don't
 * match the component ID are ignored. Fails the build if the tree has no such
 * component.
 */
std::string compose_catalog(
    const fs::path &tree,
    const std::string &prefix,
    const std::string &component_id,
    const fs::path &icons_dir,
    const fs::path &media_dir);

} // namespace mfb

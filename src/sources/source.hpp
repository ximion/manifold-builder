/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <array>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "manifest.hpp"
#include "recipe.hpp"
#include "utils.hpp"

namespace mfb
{

/**
 * Source types we know how to fetch.
 */
constexpr std::array<std::string_view, 5> SOURCE_TYPES = {"git", "archive", "file", "dir", "patch"};

/**
 * Directories a source fetcher works with.
 */
struct FetchContext {
    /// dir of the file that defines the part, local paths are relative to it
    fs::path dir;
    /// scratch space for temporary checkouts and unpacking
    fs::path scratch;
    /// downloads and git mirrors, which may be kept for later builds
    fs::path cache;
    /// our own paths, which a dir source leaves out: the work dir and the output
    std::vector<fs::path> skip;
};

/**
 * Fetches one type of source into a part's source directory.
 */
class SourceFetcher
{
public:
    virtual ~SourceFetcher() = default;

    /**
     * Validate the keys of a source and return an error message, or an empty
     * string if they are fine.
     */
    virtual std::string check(const Source &source) const = 0;

    /**
     * List the programs that fetch() runs.
     */
    virtual std::vector<std::string> tools(const Source &source) const = 0;

    /**
     * Fetch `source` into `destination` and return a record of what was fetched.
     *
     * The destination may already contain the sources listed before this one.
     */
    virtual SourceRecord fetch(const Source &source, const fs::path &destination, const FetchContext &context)
        const = 0;
};

/**
 * Get the fetcher for a source type. Fails the build for unknown types.
 */
std::unique_ptr<SourceFetcher> source_fetcher_for(const std::string &type);

/**
 * Validate a source and return an error message, or an empty string if it is fine.
 */
std::string check_source(const Source &source);

/**
 * Check that a source sets no keys its type does not use, and return an error
 * message if it does.
 *
 * `allowed` lists the keys the type reads besides `type`, `dest` and
 * `strip-components`. We leave `strip-components` alone, because it always
 * has a value.
 */
std::string check_source_keys(const Source &source, std::initializer_list<std::string_view> allowed);

/**
 * Fail the build if a program needed to fetch the sources of `parts` is missing.
 */
void check_source_tools(const std::vector<const Part *> &parts);

/**
 * Fetch the sources of all parts into one directory per part below
 * `sources_dir`, and return their records for the manifest.
 *
 * Every part gets `context`, with the `dir` of the part.
 */
std::vector<SourceRecord> fetch_sources(
    const std::vector<const Part *> &parts,
    const fs::path &sources_dir,
    FetchContext context);

/**
 * Download or locate the file of an archive, file or patch source, and return its path.
 *
 * Downloads go to the cache, named by their checksum. Fails the build if the
 * file does not match the checksum given in the source.
 */
fs::path obtain_file(const Source &source, const FetchContext &context);

/**
 * Get the file name a source's path or URL ends in, without the query or
 * fragment of a URL.
 */
std::string source_file_name(const Source &source);

/**
 * Create the build record of a source that is a single file.
 */
SourceRecord file_record(const Source &source, const fs::path &file);

} // namespace mfb

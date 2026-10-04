/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sources/source.hpp"

#include "error.hpp"
#include "identity.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "sources/archive.hpp"
#include "sources/dir.hpp"
#include "sources/file.hpp"
#include "sources/git.hpp"
#include "sources/patch.hpp"

#include <algorithm>
#include <set>

namespace mfb
{

quill::Logger *const logSources = log::logger("sources");

std::unique_ptr<SourceFetcher> source_fetcher_for(const std::string &type)
{
    if (type == "git")
        return std::make_unique<GitSource>();
    if (type == "archive")
        return std::make_unique<ArchiveSource>();
    if (type == "file")
        return std::make_unique<FileSource>();
    if (type == "dir")
        return std::make_unique<DirSource>();
    if (type == "patch")
        return std::make_unique<PatchSource>();
    fail("unknown source type '{}'", type);
}

std::string check_source(const Source &source)
{
    if (!std::ranges::contains(SOURCE_TYPES, source.type))
        return std::format("source type must be one of {}, not '{}'", join(SOURCE_TYPES, ", "), source.type);
    return source_fetcher_for(source.type)->check(source);
}

std::string check_source_keys(const Source &source, std::initializer_list<std::string_view> allowed)
{
    const std::pair<std::string_view, const std::string &> keys[] = {
        {"url", source.url},
        {"path", source.path},
        {"tag", source.tag},
        {"branch", source.branch},
        {"commit", source.commit},
        {"sha256", source.sha256},
        {"dest-filename", source.dest_filename},
    };
    for (const auto &[name, value] : keys) {
        if (!value.empty() && !std::ranges::contains(allowed, name))
            return std::format("a {} source does not take '{}'", source.type, name);
    }
    return {};
}

void check_source_tools(const std::vector<const Part *> &parts)
{
    std::set<std::string> tools;
    for (const auto *part : parts) {
        for (const auto &source : part->sources) {
            auto needed = source_fetcher_for(source.type)->tools(source);
            tools.insert(needed.begin(), needed.end());
        }
    }
    for (const auto &tool : tools)
        require_tool(tool);
}

std::vector<SourceRecord> fetch_sources(
    const std::vector<const Part *> &parts,
    const fs::path &sources_dir,
    const fs::path &scratch)
{
    std::vector<SourceRecord> records;
    for (const auto *part : parts) {
        // a part without sources still gets an empty directory to build in
        auto part_dir = sources_dir / part->name;
        fs::create_directories(part_dir);
        for (const auto &source : part->sources) {
            auto destination = part_dir / source.dest;
            auto record = source_fetcher_for(source.type)
                              ->fetch(source, destination, {.dir = part->dir, .scratch = scratch});
            record.part = part->name;
            records.push_back(record);
        }
    }
    return records;
}

fs::path obtain_file(const Source &source, const FetchContext &context)
{
    fs::path file;
    if (!source.url.empty()) {
        // name downloads by checksum (check() requires one for URLs), so we
        // fetch a file shared by several sources only once
        file = context.scratch / "downloads" / source.sha256;
        if (!fs::exists(file)) {
            fs::create_directories(file.parent_path());
            LOG_INFO(logSources, "Downloading {}...", source.url);
            auto partial = file;
            partial += ".part";
            std::vector<std::string> curl = {
                "curl",
                "--fail",
                "--silent",
                "--show-error",
                "--location",
                "--retry",
                "3",
                "--output",
                partial.string(),
                source.url,
            };
            run({.argv = curl});
            fs::rename(partial, file);
        }
    } else {
        file = context.dir / source.path;
        if (!fs::is_regular_file(file))
            fail("{} does not exist", file.string());
    }
    if (!source.sha256.empty()) {
        auto sha256 = checksum_file_hex(file, ChecksumKind::Sha256);
        if (sha256 != source.sha256)
            fail("{} has sha256 {}, not {}", source.url.empty() ? file.string() : source.url, sha256, source.sha256);
    }
    return file;
}

SourceRecord file_record(const Source &source, const fs::path &file)
{
    SourceRecord record{.type = source.type, .sha256 = checksum_file_hex(file, ChecksumKind::Sha256)};
    if (!source.url.empty())
        record.url = source.url;
    else
        record.path = source.path;
    return record;
}

} // namespace mfb

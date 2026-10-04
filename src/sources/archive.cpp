/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sources/archive.hpp"

#include "error.hpp"
#include "process.hpp"

namespace mfb
{

namespace
{

bool is_zip(const Source &source)
{
    return (source.url.empty() ? source.path : source.url).ends_with(".zip");
}

/**
 * Move the contents of `from` into `to`, dropping the first `strip` components
 * of every path.
 *
 * Like tar, we skip entries that have no more than `strip` components.
 */
void move_stripped(const fs::path &from, const fs::path &to, int strip)
{
    // Collect all entries first. When we move entries out of a directory while
    // reading it, it is unspecified what readdir() returns next.
    std::vector<fs::directory_entry> entries(fs::recursive_directory_iterator(from), {});
    for (const auto &entry : entries) {
        auto relative = entry.path().lexically_relative(from);
        if (std::distance(relative.begin(), relative.end()) <= strip)
            continue;
        fs::path stripped;
        for (auto component = std::next(relative.begin(), strip); component != relative.end(); ++component)
            stripped /= *component;
        if (entry.is_directory() && !entry.is_symlink()) {
            fs::create_directories(to / stripped);
        } else {
            fs::create_directories((to / stripped).parent_path());
            fs::rename(entry.path(), to / stripped);
        }
    }
}

} // namespace

std::string ArchiveSource::check(const Source &source) const
{
    if (source.url.empty() == source.path.empty())
        return "an archive source needs either a 'url' or a 'path'";
    if (!source.url.empty() && source.sha256.empty())
        return std::format("the archive {} needs a 'sha256'", source.url);
    if (source.strip_components < 0)
        return "'strip-components' must not be negative";
    return check_source_keys(source, {"url", "path", "sha256"});
}

std::vector<std::string> ArchiveSource::tools(const Source &source) const
{
    std::vector<std::string> tools = {is_zip(source) ? "unzip" : "tar"};
    if (!source.url.empty())
        tools.push_back("curl");
    return tools;
}

SourceRecord ArchiveSource::fetch(const Source &source, const fs::path &destination, const FetchContext &context) const
{
    auto archive = obtain_file(source, context);
    fs::create_directories(destination);
    if (is_zip(source)) {
        // unzip can not strip path components, so we do it ourselves
        auto unpacked = context.scratch / "unzip";
        fs::remove_all(unpacked);
        fs::create_directories(unpacked);
        run({.argv = {"unzip", "-q", archive.string(), "-d", unpacked.string()}});
        move_stripped(unpacked, destination, source.strip_components);
        fs::remove_all(unpacked);
    } else {
        std::vector<std::string> tar = {
            "tar",
            "-xf",
            archive.string(),
            "-C",
            destination.string(),
            "--no-same-owner",
            std::format("--strip-components={}", source.strip_components),
        };
        run({.argv = tar});
    }
    return file_record(source, archive);
}

} // namespace mfb

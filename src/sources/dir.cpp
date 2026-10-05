/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sources/dir.hpp"

#include "error.hpp"
#include "identity.hpp"
#include "process.hpp"

#include <algorithm>

namespace mfb
{

std::string DirSource::check(const Source &source) const
{
    if (source.path.empty())
        return "a dir source needs a 'path'";
    return check_source_keys(source, {"path"});
}

std::vector<std::string> DirSource::tools(const Source &) const
{
    return {"tar"};
}

SourceRecord DirSource::fetch(const Source &source, const fs::path &destination, const FetchContext &context) const
{
    auto dir = context.dir / source.path;
    if (!fs::is_directory(dir))
        fail("{} is not a directory", dir.string());
    // compare canonical paths, the walk below does not follow links
    dir = fs::canonical(dir);
    std::vector<fs::path> own;
    for (const auto &path : context.skip)
        own.push_back(fs::weakly_canonical(path));
    // hidden files are no sources, and what is in .git changes with every git command
    auto skip = [&](const fs::path &path) {
        return path.filename().string().starts_with('.') || std::ranges::contains(own, path) || is_work_dir(path);
    };

    std::string list;
    for (const auto &path : list_tree(dir, skip)) {
        list += "./" + path.string();
        list += '\0';
    }
    fs::create_directories(context.scratch);
    auto list_file = context.scratch / "dir-source.list";
    write_file(list_file, list);
    fs::create_directories(destination);
    // tar keeps modes and times, and sets those of a directory after its contents
    const char *copy = R"sh(
set -o pipefail
tar -C "$1" --create --file=- --format=posix --no-recursion --null --files-from="$2" \
    | tar -C "$3" --extract --file=- --preserve-permissions
)sh";
    run({.argv = {"bash", "-c", copy, "bash", dir.string(), list_file.string(), destination.string()}});
    fs::remove(list_file);
    return SourceRecord{.type = "dir", .path = source.path, .tree_hash = tree_hash(dir, skip)};
}

} // namespace mfb

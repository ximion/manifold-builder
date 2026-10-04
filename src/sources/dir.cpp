/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sources/dir.hpp"

#include "error.hpp"
#include "identity.hpp"
#include "process.hpp"

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
    return {"cp"};
}

SourceRecord DirSource::fetch(const Source &source, const fs::path &destination, const FetchContext &context) const
{
    auto dir = context.dir / source.path;
    if (!fs::is_directory(dir))
        fail("{} is not a directory", dir.string());
    fs::create_directories(destination);
    run({.argv = {"cp", "-a", "--reflink=auto", (dir / ".").string(), destination.string()}});
    return SourceRecord{.type = "dir", .path = source.path, .tree_hash = tree_hash(dir)};
}

} // namespace mfb

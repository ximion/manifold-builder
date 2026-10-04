/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sources/patch.hpp"

#include "error.hpp"
#include "logging.hpp"
#include "process.hpp"

namespace mfb
{

quill::Logger *const logSources = log::logger("sources");

std::string PatchSource::check(const Source &source) const
{
    if (source.path.empty())
        return "a patch source needs a 'path'";
    if (source.strip_components < 0)
        return "'strip-components' must not be negative";
    return check_source_keys(source, {"path", "sha256"});
}

std::vector<std::string> PatchSource::tools(const Source &) const
{
    return {"patch"};
}

SourceRecord PatchSource::fetch(const Source &source, const fs::path &destination, const FetchContext &context) const
{
    auto patch = obtain_file(source, context);
    if (!fs::is_directory(destination))
        fail("{} has nothing to apply to: a patch source goes after the sources it patches", patch.string());
    LOG_INFO(logSources, "applying {}...", source.path);
    // with --forward, a patch that seems applied already is an error and is
    // never reversed; --batch because nobody is there to answer questions
    std::vector<std::string> argv = {
        "patch",
        "--batch",
        "--forward",
        "--quiet",
        std::format("-p{}", source.strip_components),
        "-d",
        destination.string(),
        "-i",
        fs::absolute(patch).string(),
    };
    run({.argv = argv});
    return file_record(source, patch);
}

} // namespace mfb

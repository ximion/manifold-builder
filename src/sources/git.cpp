/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sources/git.hpp"

#include "error.hpp"
#include "logging.hpp"
#include "process.hpp"

namespace mfb
{

quill::Logger *const logSources = log::logger("sources");

std::string GitSource::check(const Source &source) const
{
    if (source.url.empty())
        return "a git source needs a 'url'";
    return check_source_keys(source, {"url", "tag", "branch", "commit"});
}

std::vector<std::string> GitSource::tools(const Source &) const
{
    return {"git"};
}

SourceRecord GitSource::fetch(const Source &source, const fs::path &destination, const FetchContext &context) const
{
    // git only clones into an empty directory, so elsure we have one
    bool empty = !fs::exists(destination) || fs::is_empty(destination);
    auto checkout = empty ? destination : context.scratch / "git-checkout";
    if (!empty)
        fs::remove_all(checkout);

    LOG_INFO(logSources, "Cloning {}...", source.url);
    run({.argv = {"git", "clone", "--quiet", source.url, checkout.string()}});

    // a commit wins over a tag, a tag over a branch; if both a tag and a
    // commit are given, the tag must point at that commit (checked below)
    std::string ref = !source.commit.empty() ? source.commit : !source.tag.empty() ? source.tag : source.branch;
    if (!ref.empty())
        run({.argv = {"git", "-C", checkout.string(), "checkout", "--quiet", ref}});
    run({.argv = {"git", "-C", checkout.string(), "submodule", "update", "--init", "--recursive", "--quiet"}});

    auto commit = run_capture({.argv = {"git", "-C", checkout.string(), "rev-parse", "HEAD"}});
    if (!source.commit.empty() && !source.tag.empty()) {
        auto tagged = run_capture({.argv = {"git", "-C", checkout.string(), "rev-parse", source.tag + "^{commit}"}});
        if (tagged != commit)
            fail("tag {} of {} is commit {}, not {}", source.tag, source.url, tagged, source.commit);
    }
    if (!empty)
        run({.argv = {"cp", "-a", "--reflink=auto", (checkout / ".").string(), destination.string()}});
    return SourceRecord{.type = "git", .url = source.url, .commit = commit};
}

} // namespace mfb

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sources/git.hpp"

#include "error.hpp"
#include "checksum.hpp"
#include "logging.hpp"
#include "process.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace mfb
{

quill::Logger *const logSources = log::logger("sources");

std::string GitSource::check(const Source &source) const
{
    if (source.url.empty())
        return "a git source needs a 'url'";
    return check_source_keys(source, {"url", "tag", "branch", "commit"});
}

namespace
{

/**
 * An exclusive lock on a file, held while the object lives.
 */
class FileLock
{
public:
    explicit FileLock(const fs::path &path)
        : m_fd(open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644))
    {
        if (m_fd >= 0 && flock(m_fd, LOCK_EX) == 0)
            return;
        auto error = errno;
        if (m_fd >= 0)
            close(m_fd);
        fail("Cannot lock {}: {}", path.string(), std::strerror(error));
    }

    ~FileLock()
    {
        close(m_fd);
    }

    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

private:
    int m_fd;
};

/**
 * Create or update the bare mirror of a git source.
 *
 * A mirror that has everything a source pins is not updated, so such a
 * source needs no network.
 */
void update_mirror(const Source &source, const fs::path &mirror)
{
    if (!fs::exists(mirror)) {
        LOG_INFO(logSources, "Cloning {}...", source.url);
        auto partial = mirror;
        partial += ".part";
        fs::remove_all(partial);
        run({.argv = {"git", "clone", "--quiet", "--bare", source.url, partial.string()}});
        // a bare clone does not say what later fetches update
        run({.argv = {"git", "-C", partial.string(), "config", "remote.origin.fetch", "+refs/heads/*:refs/heads/*"}});
        fs::rename(partial, mirror);
        return;
    }

    auto has = [&](const std::string &revision) {
        return run_status(
                   {.argv = {"git", "-C", mirror.string(), "rev-parse", "--verify", "--quiet", revision},
                    .quiet = true})
               == 0;
    };
    bool pinned = !source.commit.empty() && has(source.commit + "^{commit}")
                  && (source.tag.empty() || has("refs/tags/" + source.tag));
    if (pinned)
        return;
    LOG_INFO(logSources, "Updating {}...", source.url);
    run({.argv = {"git", "-C", mirror.string(), "fetch", "--quiet", "--prune", "--tags", "origin"}});
}

} // namespace

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

    // one bare mirror per URL, which parallel builds take turns with
    auto mirror = context.cache / "git" / (checksum_data_hex(source.url, ChecksumKind::Sha256).substr(0, 32) + ".git");
    fs::create_directories(mirror.parent_path());
    {
        FileLock lock(mirror.string() + ".lock");
        // keeps it from being pruned
        if (fs::exists(mirror))
            touch(mirror);
        update_mirror(source, mirror);
        run({.argv = {"git", "clone", "--quiet", mirror.string(), checkout.string()}});
    }
    // relative submodule URLs, and the part's own git commands, need the real origin
    run({.argv = {"git", "-C", checkout.string(), "remote", "set-url", "origin", source.url}});

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
            fail("Tag {} of {} is commit {}, not {}", source.tag, source.url, tagged, source.commit);
    }
    if (!empty)
        run({.argv = {"cp", "-a", "--reflink=auto", (checkout / ".").string(), destination.string()}});
    return SourceRecord{.type = "git", .url = source.url, .commit = commit};
}

} // namespace mfb

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mfb
{

namespace fs = std::filesystem;

/**
 * Strip leading and trailing whitespace from a text.
 */
std::string_view trim(std::string_view text);

/**
 * Call `line_fn` for every line of `text`, without the line break.
 */
void for_each_line(std::string_view text, auto &&line_fn)
{
    for (size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        line_fn(text.substr(start, end - start));
        start = end + 1;
    }
}

/**
 * Read the contents of a file in text lines.
 */
std::vector<std::string> read_lines(const fs::path &path);

/**
 * Join the entries into one string, with `separator` between them.
 */
template<std::ranges::input_range Range>
std::string join(Range &&entries, std::string_view separator)
{
    std::string joined;
    bool first = true;
    for (const auto &entry : entries) {
        if (!first)
            joined += separator;
        joined += entry;
        first = false;
    }
    return joined;
}

/**
 * Read the contents of a file, or fail the build if it can not be read.
 */
std::string read_file(const fs::path &path);

/**
 * Write a file, replacing an existing one, or fail the build if it can not
 * be written.
 */
void write_file(const fs::path &path, std::string_view content);

/**
 * Set the modification time of a file or directory to now, if we can.
 */
void touch(const fs::path &path);

/**
 * An exclusive lock on a file or directory, held while the object lives.
 *
 * A missing path is created as a file.
 */
class FileLock
{
public:
    /**
     * Wait for the lock on `path`, or fail the build if it can not be taken.
     */
    explicit FileLock(const fs::path &path);

    /**
     * Take the lock on `path` if nobody else holds it, or return nothing.
     * Other errors fail the build.
     */
    static std::optional<FileLock> try_lock(const fs::path &path);

    FileLock(FileLock &&other) noexcept;
    FileLock &operator=(FileLock &&other) noexcept;
    ~FileLock();

    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

private:
    explicit FileLock(int fd);

    int m_fd;
};

/**
 * Parse a whole string as a decimal integer, or return nothing.
 */
std::optional<int64_t> parse_int(std::string_view text);

/**
 * List everything below `dir`, relative to it, in no particular order.
 *
 * An entry `skip` returns true for is left out, with all it contains.
 */
std::vector<fs::path> list_tree(const fs::path &dir, const std::function<bool(const fs::path &)> &skip = {});

/**
 * The .gitignore of every work directory, which also marks it as ours.
 */
constexpr std::string_view WORK_DIR_GITIGNORE_DATA = "# manifold-builder work directory\n*\n";

/**
 * Check if a directory is a work directory of ours, by its .gitignore.
 */
bool is_work_dir(const fs::path &dir);

/**
 * Remove the files below `dir` that `reference` has unchanged at the same path.
 *
 * Files are unchanged if type, mode, owner and content, or the target of a
 * symbolic link, are the same.
 * Below `stat_only_dirs` (relative to both), we only compare size and mtime,
 * and skip the full content comparison.
 * Directories and anything that can't be read or removed are kept.
 *
 * Returns the number of files removed.
 */
size_t remove_identical_files(
    const fs::path &dir,
    const fs::path &reference,
    const std::set<std::string> &stat_only_dirs = {});

/**
 * Join the non-empty entries with ':' into a search path.
 */
std::string make_search_path(std::initializer_list<std::string> entries);

/**
 * Get the current time in UTC as ISO 8601 text.
 */
std::string utc_now();

/**
 * Compute a revision the builder gives a build at `epoch`.
 *
 * The revision is one number made of the years since 2024, the day of the
 * year in three digits and the time of day in 10-second slots in four digits,
 * all in UTC (22880949 for 2026-10-15T02:38:17Z). This way a later build gets
 * a higher revision without any repository involved. A repository may
 * replace it with a count of its own.
 */
int64_t build_revision_from_time(int64_t epoch);

} // namespace mfb

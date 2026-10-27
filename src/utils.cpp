/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "utils.hpp"

#include "error.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <set>
#include <sys/stat.h>

namespace mfb
{

namespace
{

/**
 * Check if two regular files of the same size have the same content.
 */
bool same_content(const fs::path &path, const fs::path &other)
{
    std::ifstream first(path, std::ios::binary);
    std::ifstream second(other, std::ios::binary);
    std::vector<char> left(1 << 16);
    std::vector<char> right(1 << 16);
    while (first && second) {
        first.read(left.data(), static_cast<std::streamsize>(left.size()));
        second.read(right.data(), static_cast<std::streamsize>(right.size()));
        if (first.gcount() != second.gcount()
            || !std::equal(left.begin(), left.begin() + first.gcount(), right.begin()))
            return false;
    }
    // a read error is not the end of both files
    return first.eof() && second.eof();
}

/**
 * Check if a file is unchanged at `other`, by its status alone if `stat_only` is set.
 */
bool same_file(const fs::path &path, const fs::path &other, bool stat_only)
{
    struct stat a;
    struct stat b;
    if (lstat(path.c_str(), &a) != 0 || lstat(other.c_str(), &b) != 0)
        return false;
    // the mode includes the file type
    if (a.st_mode != b.st_mode || a.st_uid != b.st_uid || a.st_gid != b.st_gid)
        return false;
    if (S_ISLNK(a.st_mode)) {
        std::error_code error;
        auto target = fs::read_symlink(path, error);
        if (error)
            return false;
        auto other_target = fs::read_symlink(other, error);
        return !error && target == other_target;
    }
    // whiteouts and other special files stay
    if (!S_ISREG(a.st_mode) || a.st_size != b.st_size)
        return false;
    if (stat_only)
        return a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
    return same_content(path, other);
}

/**
 * Check if a relative path lies below one of `dirs`.
 */
bool is_below(const fs::path &relative, const std::set<std::string> &dirs)
{
    return std::ranges::any_of(dirs, [&](const std::string &dir) {
        fs::path prefix(dir);
        return std::ranges::mismatch(prefix, relative).in1 == prefix.end();
    });
}

} // namespace

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::string read_file(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        fail("Cannot read {}: {}", path.string(), std::strerror(errno));
    // a read error, e.g. of a directory, sets badbit, the end of the file does not
    std::string content;
    char buffer[65536];
    while (file.read(buffer, sizeof buffer) || file.gcount() > 0)
        content.append(buffer, static_cast<size_t>(file.gcount()));
    if (file.bad())
        fail("Cannot read {}: {}", path.string(), std::strerror(errno));
    return content;
}

void write_file(const fs::path &path, std::string_view content)
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
        fail("Cannot write {}: {}", path.string(), std::strerror(errno));
    file << content;
    // a full disk only shows once the data is flushed
    file.close();
    if (!file)
        fail("Cannot write {}: {}", path.string(), std::strerror(errno));
}

void touch(const fs::path &path)
{
    std::error_code error;
    fs::last_write_time(path, fs::file_time_type::clock::now(), error);
}

std::optional<int64_t> parse_int(std::string_view text)
{
    int64_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

std::vector<fs::path> list_tree(const fs::path &dir, const std::function<bool(const fs::path &)> &skip)
{
    std::vector<fs::path> entries;
    for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator(); ++it) {
        if (skip && skip(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        entries.push_back(it->path().lexically_relative(dir));
    }
    return entries;
}

bool is_work_dir(const fs::path &dir)
{
    auto gitignore = dir / ".gitignore";
    std::error_code error;
    if (!fs::is_regular_file(gitignore, error) || fs::file_size(gitignore, error) != WORK_DIR_GITIGNORE_DATA.size())
        return false;
    return read_file(gitignore) == WORK_DIR_GITIGNORE_DATA;
}

size_t remove_identical_files(
    const fs::path &dir,
    const fs::path &reference,
    const std::set<std::string> &stat_only_dirs)
{
    // collect first, removing entries while we walk would disturb the walk
    std::vector<fs::path> identical;
    std::error_code error;
    for (fs::recursive_directory_iterator entry(dir, fs::directory_options::skip_permission_denied, error), end;
         !error && entry != end;
         entry.increment(error)) {
        std::error_code type_error;
        if (entry->symlink_status(type_error).type() == fs::file_type::directory)
            continue;
        auto relative = entry->path().lexically_relative(dir);
        if (same_file(entry->path(), reference / relative, is_below(relative, stat_only_dirs)))
            identical.push_back(entry->path());
    }

    size_t removed = 0;
    for (const auto &path : identical) {
        if (fs::remove(path, error))
            ++removed;
    }
    return removed;
}

std::string make_search_path(std::initializer_list<std::string> entries)
{
    return join(
        entries | std::views::filter([](const std::string &entry) {
            return !entry.empty();
        }),
        ":");
}

std::vector<std::string> read_lines(const fs::path &path)
{
    std::vector<std::string> lines;
    for_each_line(read_file(path), [&](std::string_view line) {
        if (!line.empty())
            lines.emplace_back(line);
    });
    return lines;
}

std::string utc_now()
{
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%TZ}", now);
}

int64_t build_revision_from_time(int64_t epoch)
{
    using namespace std::chrono;
    sys_seconds when{seconds{epoch}};
    auto day = floor<days>(when);
    year_month_day date{day};
    auto day_of_year = (day - sys_days{date.year() / January / 1}).count() + 1;
    // time of day in 10-second slots, always four digits, so the number and
    // its decimal text sort the same way
    auto slot = (when - day).count() / 10;
    return ((static_cast<int>(date.year()) - 2024) * 1000 + day_of_year) * 10000 + slot;
}

} // namespace mfb

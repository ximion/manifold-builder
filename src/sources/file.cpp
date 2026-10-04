/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "sources/file.hpp"

namespace mfb
{

std::string FileSource::check(const Source &source) const
{
    if (source.url.empty() == source.path.empty())
        return "a file source needs either a 'url' or a 'path'";
    if (!source.url.empty() && source.sha256.empty())
        return std::format("the file {} needs a 'sha256'", source.url);
    if (source.dest_filename.find('/') != std::string::npos)
        return std::format("'dest-filename' must be a file name, not '{}'", source.dest_filename);
    return check_source_keys(source, {"url", "path", "sha256", "dest-filename"});
}

std::vector<std::string> FileSource::tools(const Source &source) const
{
    if (!source.url.empty())
        return {"curl"};
    return {};
}

SourceRecord FileSource::fetch(const Source &source, const fs::path &destination, const FetchContext &context) const
{
    auto file = obtain_file(source, context);
    auto name = source.dest_filename;
    if (name.empty()) {
        // use the last path component, without query or fragment
        std::string_view from = source.url.empty() ? source.path : source.url;
        from = from.substr(0, from.find_first_of("?#"));
        name = fs::path(from).filename().string();
    }
    fs::create_directories(destination);
    fs::copy_file(file, destination / name, fs::copy_options::overwrite_existing);
    return file_record(source, file);
}

} // namespace mfb

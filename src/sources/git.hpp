/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "sources/source.hpp"

namespace mfb
{

/**
 * Source that clones a git repository with its submodules, at a commit, tag
 * or branch.
 */
class GitSource : public SourceFetcher
{
public:
    std::string check(const Source &source) const override;
    std::vector<std::string> tools(const Source &source) const override;
    SourceRecord fetch(const Source &source, const fs::path &destination, const FetchContext &context) const override;
};

} // namespace mfb

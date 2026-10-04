/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "buildsystems/simple.hpp"

namespace mfb
{

std::string SimpleBuildSystem::build(
    const Part &,
    const InstallTarget &,
    const std::vector<std::string> &,
    const fs::path &) const
{
    return {};
}

std::string SimpleBuildSystem::install(const Part &) const
{
    return {};
}

} // namespace mfb

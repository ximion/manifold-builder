/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "buildsystems/buildsystem.hpp"

namespace mfb
{

class MesonBuildSystem : public BuildSystem
{
public:
    std::string build(
        const Part &part,
        const InstallTarget &target,
        const std::vector<std::string> &config_opts,
        const fs::path &source_dir) const override;
    std::string install(const Part &part) const override;
};

} // namespace mfb

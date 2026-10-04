/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "buildsystems/buildsystem.hpp"

namespace mfb
{

/**
 * Builds a part with CMake out of tree, generating for Ninja ("cmake") or
 * make ("cmake-make").
 */
class CMakeBuildSystem : public BuildSystem
{
public:
    enum class Generator {
        Ninja,
        Make,
    };

    explicit CMakeBuildSystem(Generator generator);

    std::string build(
        const Part &part,
        const InstallTarget &target,
        const std::vector<std::string> &config_opts,
        const fs::path &source_dir) const override;
    std::string install(const Part &part) const override;

private:
    /**
     * Get the build tool for the generator (ninja or make).
     */
    std::string build_tool() const;

    Generator m_generator;
};

} // namespace mfb

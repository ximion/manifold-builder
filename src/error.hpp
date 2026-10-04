/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <format>
#include <stdexcept>
#include <string>
#include <utility>

namespace mfb
{

/**
 * Error that ends the build, with a message for the user.
 */
class BuildError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/**
 * End the build with a formatted error message.
 */
template<typename... Args>
[[noreturn]] void fail(std::format_string<Args...> fmt, Args &&...args)
{
    throw BuildError(std::format(fmt, std::forward<Args>(args)...));
}

} // namespace mfb

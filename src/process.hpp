/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "utils.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mfb
{

/**
 * A program to run, and how to run it.
 *
 * Every command gets TZ=UTC and LC_ALL=C on top of the builder's own
 * environment. debuerreotype's scripts force the same, so date(1) and the
 * scripts agree on what a timestamp means.
 */
struct Command {
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
    std::optional<fs::path> cwd;
    bool quiet = false; /// Discard what the command writes to stderr
};

/**
 * Run the command and wait for it to finish.
 *
 * Fails the build if the command can not be started or exits with a non-zero
 * status.
 */
void run(const Command &command);

/**
 * Run the command and return what it wrote to stdout.
 *
 * Surrounding whitespace is removed from the output.
 * Failures fail the build.
 */
std::string run_capture(const Command &command);

/**
 * Get the first line a tool prints when asked for its version, or "unknown".
 */
std::string tool_version(std::vector<std::string> argv);

/**
 * Run the command and return its exit status instead of failing.
 *
 * Returns -1 if the program could not be started at all.
 */
int run_status(const Command &command);

/**
 * Quote an argument so a shell reads it back unchanged.
 */
std::string shell_quote(std::string_view argument);

/**
 * Join an argument vector into a line that a shell reads back as the same
 * arguments.
 *
 * We use this for logging and to generate shell scripts.
 */
std::string shell_join(std::span<const std::string> argv);

/**
 * Join shell commands into the text of a script, with a newline after each.
 */
std::string shell_lines(const std::vector<std::string> &commands);

/**
 * Look up a program on PATH.
 */
std::optional<fs::path> find_tool(std::string_view name);

/**
 * Look up a program on PATH, and fail the build if it is missing.
 */
fs::path require_tool(std::string_view name);

} // namespace mfb

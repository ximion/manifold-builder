/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <quill/LogMacros.h>
#include <quill/Logger.h>

#include <string_view>

namespace mfb::log
{

/**
 * Set up logging. Call this once at program start.
 */
void init(bool verbose);
bool verbose();

/**
 * Get the logger for an area of the builder, creating it on first use.
 * Its name is part of every line it writes.
 */
quill::Logger *logger(std::string_view name);

/**
 * Get the logger for messages that belong to no particular area.
 */
quill::Logger *root();

/**
 * Print a header box with a title.
 */
void header_box(std::string_view title);

/**
 * Print a box with a title for a build step.
 */
void section_box(std::string_view title);

/**
 * Wait until all messages logged so far are written.
 * We call this before running a subprocess that writes to the terminal,
 * so that the outputs do not interleave.
 */
void flush();

} // namespace mfb::log

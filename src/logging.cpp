/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "logging.hpp"

#include <chrono>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>
#include <string>

namespace mfb::log
{

namespace
{

constexpr const char *MFB_LOG_PATTERN = "%(time) %(log_level_short_code) %(logger): %(message)";

bool g_verbose = false;

quill::LogLevel level_for(bool verbose)
{
    return verbose ? quill::LogLevel::Debug : quill::LogLevel::Info;
}

/**
 * Get the logger for the boxes, which writes them without level or colour.
 */
quill::Logger *plain_logger()
{
    static quill::Logger *const logPlain = [] {
        quill::ConsoleSinkConfig config;
        config.set_stream("stdout");
        config.set_colour_mode(quill::ConsoleSinkConfig::ColourMode::Never);
        return quill::Frontend::create_or_get_logger(
            "plain",
            quill::Frontend::create_or_get_sink<quill::ConsoleSink>("plain", config),
            quill::PatternFormatterOptions{"%(message)"});
    }();
    return logPlain;
}

/**
 * Draw a box around a title with the given corner and line pieces.
 * Borrowed from appstream-generator.
 */
void print_text_box(
    std::string_view title,
    std::string_view top_left,
    std::string_view horizontal,
    std::string_view top_right,
    std::string_view vertical,
    std::string_view bottom_left,
    std::string_view bottom_right)
{
    const auto line_count = 10 + title.length();

    std::string output = "\n";
    output += top_left;
    for (size_t i = 0; i < line_count; ++i)
        output += horizontal;
    output += top_right;
    output += '\n';

    output += vertical;
    output += "  ";
    output += title;
    output += std::string(8, ' ');
    output += vertical;
    output += '\n';

    output += bottom_left;
    for (size_t i = 0; i < line_count; ++i)
        output += horizontal;
    output += bottom_right;

    LOG_INFO(plain_logger(), "{}", output);
}

} // namespace

void init(bool verbose)
{
    g_verbose = verbose;

    quill::BackendOptions backend;
    // allow printing UTF-8 in logs
    backend.check_printable_char = {};
    // The backend thread polls its queues every 100µs by default, which keeps a
    // CPU core busy while a build waits on apt for minutes. We only log a few
    // lines per minute, and flush() wakes the backend up anyway.
    backend.sleep_duration = std::chrono::milliseconds(10);
    quill::Backend::start(backend);

    // the loggers of the source files already exist, set their level
    for (auto *logger : quill::Frontend::get_all_loggers())
        logger->set_log_level(level_for(verbose));
}

bool verbose()
{
    return g_verbose;
}

quill::Logger *logger(std::string_view name)
{
    // works before init(), messages wait in the queue until the backend runs
    quill::ConsoleSinkConfig config;
    config.set_stream("stderr");
    auto *logger = quill::Frontend::create_or_get_logger(
        std::string(name),
        quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console", config),
        quill::PatternFormatterOptions{MFB_LOG_PATTERN, "%H:%M:%S"});
    logger->set_log_level(level_for(g_verbose));
    return logger;
}

quill::Logger *root()
{
    static quill::Logger *const logRoot = logger("main");
    return logRoot;
}

void header_box(std::string_view title)
{
    print_text_box(title, "╔", "═", "╗", "║", "╚", "╝");
}

void section_box(std::string_view title)
{
    print_text_box(title, "┌", "─", "┐", "│", "└", "┘");
}

void flush()
{
    // without a running backend, we would wait forever
    if (!quill::Backend::is_running())
        return;
    // flush_log() does not wake a sleeping backend, so do it here
    quill::Backend::notify();
    // this flushes all loggers, not just the root one
    root()->flush_log();
}

} // namespace mfb::log

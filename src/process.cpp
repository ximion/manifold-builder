/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "process.hpp"

#include "error.hpp"
#include "logging.hpp"
#include "utils.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace mfb
{

quill::Logger *const logProcess = log::logger("process");

namespace
{

/**
 * Build the environment of a child process.
 *
 * We start from our own environment and add the fixed time zone and locale
 * and then the command's own variables on top.
 */
std::vector<std::string> child_environment(const Command &command)
{
    std::map<std::string, std::string> variables;

    for (char **entry = environ; *entry != nullptr; ++entry) {
        std::string_view assignment(*entry);
        auto equals = assignment.find('=');
        if (equals == std::string_view::npos)
            continue;
        variables.emplace(assignment.substr(0, equals), assignment.substr(equals + 1));
    }

    variables["TZ"] = "UTC";
    variables["LC_ALL"] = "C";
    for (const auto &[name, value] : command.env)
        variables[name] = value;

    std::vector<std::string> result;
    for (const auto &[name, value] : variables)
        result.push_back(name + "=" + value);
    return result;
}

std::vector<char *> c_strings(std::vector<std::string> &strings)
{
    std::vector<char *> pointers;
    for (auto &s : strings)
        pointers.push_back(s.data());
    pointers.push_back(nullptr);
    return pointers;
}

struct Finished {
    int status;      // -1 if the program could not be started
    int spawn_error; // why it could not be started
    std::string output;
};

/**
 * Run the command, and collect what it writes to stdout if `capture_stdout`
 * is set.
 */
Finished spawn(const Command &command, bool capture_stdout)
{
    if (command.argv.empty())
        fail("attempted to run an empty command");

    LOG_DEBUG(logProcess, "+ {}", shell_join(command.argv));
    log::flush();

    int pipe_fds[2] = {-1, -1};
    if (capture_stdout && pipe(pipe_fds) != 0)
        fail("Cannot create pipe: {}", std::strerror(errno));

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (capture_stdout) {
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    }
    if (command.quiet)
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (command.cwd)
        posix_spawn_file_actions_addchdir_np(&actions, command.cwd->c_str());

    auto argv = command.argv;
    auto env = child_environment(command);
    auto argv_ptrs = c_strings(argv);
    auto env_ptrs = c_strings(env);

    pid_t pid = 0;
    int error = posix_spawnp(&pid, argv_ptrs[0], &actions, nullptr, argv_ptrs.data(), env_ptrs.data());
    posix_spawn_file_actions_destroy(&actions);

    if (capture_stdout)
        close(pipe_fds[1]);

    if (error != 0) {
        if (capture_stdout)
            close(pipe_fds[0]);
        LOG_DEBUG(logProcess, "Cannot start {}: {}", command.argv[0], std::strerror(error));
        return {-1, error, {}};
    }

    std::string output;
    if (capture_stdout) {
        char buffer[65536];
        for (;;) {
            ssize_t count = read(pipe_fds[0], buffer, sizeof buffer);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            output.append(buffer, static_cast<size_t>(count));
        }
        close(pipe_fds[0]);
    }

    int wait_status = 0;
    while (waitpid(pid, &wait_status, 0) < 0) {
        if (errno != EINTR)
            fail("waitpid failed: {}", std::strerror(errno));
    }

    int status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 128 + WTERMSIG(wait_status);
    return {status, 0, std::move(output)};
}

void check_finished(const Command &command, const Finished &finished)
{
    if (finished.status == -1)
        fail("Cannot run {}: {}", command.argv[0], std::strerror(finished.spawn_error));
    if (finished.status != 0)
        fail("Command failed with status {}: {}", finished.status, shell_join(command.argv));
}

} // namespace

void run(const Command &command)
{
    check_finished(command, spawn(command, false));
}

std::string run_capture(const Command &command)
{
    auto finished = spawn(command, true);
    check_finished(command, finished);
    return std::string(trim(finished.output));
}

std::string tool_version(std::vector<std::string> argv)
{
    try {
        auto text = run_capture({.argv = std::move(argv), .quiet = true});
        return text.substr(0, text.find('\n'));
    } catch (const BuildError &) {
        return "unknown";
    }
}

int run_status(const Command &command)
{
    return spawn(command, false).status;
}

std::string shell_quote(std::string_view argument)
{
    constexpr std::string_view safe = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@%+=:,./-_";
    if (!argument.empty() && argument.find_first_not_of(safe) == std::string_view::npos)
        return std::string(argument);

    std::string quoted = "'";
    for (char c : argument) {
        if (c == '\'')
            quoted += "'\"'\"'";
        else
            quoted += c;
    }
    quoted += '\'';
    return quoted;
}

std::string shell_join(std::span<const std::string> argv)
{
    std::string line;
    for (const auto &argument : argv) {
        if (!line.empty())
            line += ' ';
        line += shell_quote(argument);
    }
    return line;
}

std::string shell_lines(const std::vector<std::string> &commands)
{
    std::string text;
    for (const auto &command : commands) {
        text += command;
        text += '\n';
    }
    return text;
}

std::optional<fs::path> find_tool(std::string_view name)
{
    const char *path_variable = std::getenv("PATH");
    std::string_view path = path_variable ? path_variable : "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";

    for (size_t start = 0; start <= path.size();) {
        auto end = path.find(':', start);
        if (end == std::string_view::npos)
            end = path.size();
        fs::path candidate = fs::path(path.substr(start, end - start)) / name;
        if (!candidate.empty() && access(candidate.c_str(), X_OK) == 0)
            return candidate;
        start = end + 1;
    }
    return std::nullopt;
}

fs::path require_tool(std::string_view name)
{
    auto tool = find_tool(name);
    if (!tool)
        fail("required tool '{}' not found on PATH", name);
    return *tool;
}

} // namespace mfb

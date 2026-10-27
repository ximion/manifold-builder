/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "utils.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mfb
{

// Run shell scripts as root in a user namespace.
//
// We create the user namespace with "unshare --map-auto --map-root-user".
// Our own uid becomes 0 and the whole /etc/subuid range is mapped after it,
// so distro package managers can chown files to users such as _apt and man.
// bubblewrap runs inside that namespace and adds mount and pid isolation.
// It must not use --unshare-user, which would drop the mapping.
// Nothing here needs real privileges, and the build can not touch anything
// on the host that our own user could not write anyway.
//
// If a script fails, the error names the step of the recipe and the recipe
// command that failed. The shells write both to a report through
// REPORT_FD, which, unlike a path, reaches into chroots and bubblewrap.

/// File descriptor that scripts write their failure report to
constexpr int REPORT_FD = 77;

/**
 * A Bash script that we build line by line.
 *
 * Once the script is done, we run it in a sandbox or directly.
 */
class Script
{
public:
    Script();

    /**
     * Append a line verbatim.
     */
    void line(std::string text);

    /**
     * Append a command, quoted so the shell passes the arguments unchanged.
     */
    void run(std::vector<std::string> argv);

    /**
     * Append a comment.
     */
    void comment(std::string text);

    /**
     * Append a command that writes `content` to `path`, replacing the file.
     */
    void write_file(const fs::path &path, std::string_view content);

    /**
     * Append a command that shows `text` on stderr.
     */
    void message(std::string_view text);

    /**
     * Append what `add` appends as a step of the recipe, e.g. a part or an
     * action.
     *
     * If the script fails in the step, the error names it.
     */
    void step(std::string description, const std::function<void()> &add);

    const std::string &text() const
    {
        return m_text;
    }
    const std::vector<std::string> &steps() const
    {
        return m_steps;
    }

private:
    void begin_step(std::string description);
    void end_step();

    std::string m_text;
    std::vector<std::string> m_steps;
};

/**
 * Prepare commands from the recipe to run in a bash of their own.
 *
 * The shell prints each command before running it, and reports the one
 * that fails.
 */
std::string traced_script(std::string_view commands);

/**
 * Describe why a script failed with `status`, from its failure report.
 */
std::string describe_failure(const Script &script, std::string_view report, int status);

/**
 * Mount the filesystem of a disk image read-only at `mountpoint` using FUSE.
 *
 * `offset` is where the filesystem starts in the image.
 */
void mount_image(Script &s, const fs::path &image, uint64_t offset, const fs::path &mountpoint);

/**
 * Mount an overlay of `lowers` (top-most first) at `merged`, with `upper` as
 * the writable layer.
 *
 * Without an upper layer, the overlay is read-only.
 */
void mount_overlay(
    Script &s,
    const std::vector<fs::path> &lowers,
    const fs::path &upper,
    const fs::path &work,
    const fs::path &merged);
void mount_overlay(Script &s, const std::vector<fs::path> &lowers, const fs::path &merged);

/**
 * Remove the opaque markers overlayfs put on the directories of a former
 * upper layer.
 *
 * Without the markers, we can stack the layer on top of others without
 * hiding their contents. Packed images never carry the markers because we
 * build them without extended attributes, and this makes a live layer behave
 * the same way.
 */
void strip_overlay_markers(Script &s, const fs::path &upper);

/**
 * Copy files from the lower layers of a mounted overlay into its upper layer, unchanged.
 *
 * `paths` are relative to `merged`. Missing ones are skipped.
 */
void copy_up(Script &s, const fs::path &merged, const std::vector<std::string> &paths);

/**
 * Move the paths matching `patterns` to the same place below `destination`.
 *
 * The patterns are shell globs relative to `prefix` inside `tree`.
 */
void move_paths(
    Script &s,
    const fs::path &tree,
    std::string_view prefix,
    const std::vector<std::string> &patterns,
    const fs::path &destination);

/**
 * Move the bin, sbin and lib directories at the top of `tree` into its
 * /usr, where every tree we build has them.
 */
void merge_into_usr(Script &s, const fs::path &tree);

/**
 * The sandbox in which we build a part.
 *
 * We run bubblewrap over `root`, with the sources at /run/build/<name>, a
 * private /tmp and HOME, and none of the caller's environment. The script
 * runs in the sources, or in their `subdir`.
 */
struct PartBuild {
    fs::path root;
    fs::path sources;
    std::string name;
    std::string subdir;                                   ///< of the sources, to build in
    int64_t epoch = 0;                                    ///< SOURCE_DATE_EPOCH
    std::vector<std::pair<fs::path, std::string>> binds;  ///< host path, path inside
    std::vector<std::pair<std::string, std::string>> env; ///< after the defaults, so they win
    std::string script;
    fs::path script_file; ///< where we write the script for the sandbox to run
};

/**
 * Run the build of a part in its sandbox.
 */
void run_part_build(Script &s, const PartBuild &build);

struct Sandbox {
    /**
     * Scratch space for everything the script creates, used as $TMPDIR.
     */
    fs::path tmp_dir;

    /**
     * Run the script as root in the namespace, and fail the build if the
     * script fails.
     *
     * The script goes to a numbered file in `tmp_dir`, which bash reads.
     */
    void run(const Script &script, std::map<std::string, std::string> env = {});

    /**
     * Remove a directory tree whose files may belong to mapped subuids.
     *
     * Our own uid can not delete such files from outside the namespace.
     * We never cross into a different filesystem mounted below `path`.
     */
    void remove_tree(const fs::path &path) const;

    /**
     * Check that user namespaces with subuid mapping work here.
     */
    static void check_available();

private:
    unsigned m_scripts = 0; ///< run so far
};

} // namespace mfb

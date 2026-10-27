/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sandbox.hpp"

#include "error.hpp"
#include "logging.hpp"
#include "process.hpp"
#include "utils.hpp"

#include <charconv>
#include <ranges>

namespace mfb
{

quill::Logger *const logSandbox = log::logger("sandbox");

Script::Script()
{
    // What the scripts create goes into images, so the caller's umask must
    // not show in it. If a command fails, we report the step it belongs to.
    m_text = std::format(
        R"sh(
set -Eeuo pipefail
umask 022
export TZ=UTC LC_ALL=C
trap 'printf "step\0%s\0" "${{mfb_step-}}" >&{}' ERR
)sh",
        REPORT_FD);
}

void Script::line(std::string text)
{
    m_text += text;
    m_text += '\n';
}

void Script::run(std::vector<std::string> argv)
{
    line(shell_join(argv));
}

void Script::comment(std::string text)
{
    line("# " + text);
}

void Script::write_file(const fs::path &path, std::string_view content)
{
    line(std::format("printf '%s' {} > {}", shell_quote(content), shell_quote(path.string())));
}

void Script::message(std::string_view text)
{
    line(std::format("echo {} >&2", shell_quote(text)));
}

void Script::step(std::string description, const std::function<void()> &add)
{
    begin_step(std::move(description));
    add();
    end_step();
}

void Script::begin_step(std::string description)
{
    m_steps.push_back(std::move(description));
    line(std::format("mfb_step={}", m_steps.size() - 1));
}

void Script::end_step()
{
    line("mfb_step=");
}

std::string traced_script(std::string_view commands)
{
    // NOTE: With -E, the trap also fires in subshells, even where a failure
    // does not stop the script. We only report at the top level, where it
    // always does, so the report names the recipe's own line. The trap's
    // own commands stay out of the trace.
    return std::format(
        R"sh(
trap '{{ mfb_status=$?; [ "$BASH_SUBSHELL" -ne 0 ] || printf "command\0%s\0%s\0" "$mfb_status" "$BASH_COMMAND" >&{}; }} 2>/dev/null' ERR
set -x
{}
)sh",
        REPORT_FD,
        commands);
}

std::string describe_failure(const Script &script, std::string_view report, int status)
{
    // The report is a list of NUL-terminated fields. A traced script adds
    // "command", the status and the command, the script itself "step" and
    // the index of the step.
    std::vector<std::string_view> fields;
    for (auto field : std::views::split(report, '\0'))
        fields.emplace_back(field.begin(), field.end());

    std::string_view command;
    std::string_view command_status;
    std::string step;
    for (size_t i = 0; i < fields.size();) {
        if (fields[i] == "command" && i + 2 < fields.size()) {
            command_status = fields[i + 1];
            command = fields[i + 2];
            i += 3;
        } else if (fields[i] == "step" && i + 1 < fields.size()) {
            auto text = fields[i + 1];
            size_t index = 0;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), index);
            bool valid = error == std::errc() && end == text.data() + text.size() && index < script.steps().size();
            step = valid ? script.steps()[index] : "";
            i += 2;
        } else {
            break;
        }
    }

    if (!command.empty()) {
        auto failed = std::format("Command failed with status {}: {}", command_status, trim(command));
        return step.empty() ? failed : std::format("{}: {}", step, failed);
    }
    if (!step.empty())
        return std::format("{} failed with status {}", step, status);
    return std::format("Build script failed with status {} (run with --verbose to see the script)", status);
}

namespace
{

/**
 * Create a command that runs as root in a fresh user namespace.
 *
 * Our own uid becomes 0 and the subuid range is mapped after it.
 */
Command in_namespace(std::vector<std::string> argv)
{
    Command command;
    command.argv = {
        require_tool("unshare").string(),
        "--user",
        "--map-auto",
        "--map-root-user",
    };
    for (auto &argument : argv)
        command.argv.push_back(std::move(argument));
    return command;
}

} // namespace

void mount_image(Script &s, const fs::path &image, uint64_t offset, const fs::path &mountpoint)
{
    s.run({"mkdir", "-p", mountpoint.string()});
    // errors only, not a banner and a warning on every mount
    s.run({"erofsfuse", "--dbglevel=0", std::format("--offset={}", offset), image.string(), mountpoint.string()});
}

void mount_overlay(
    Script &s,
    const std::vector<fs::path> &lowers,
    const fs::path &upper,
    const fs::path &work,
    const fs::path &merged)
{
    // NOTE: We need one lowerdir+= per layer. mount(8) passes every option
    // value through fsconfig(2), which limits a value to 256 bytes, and two
    // absolute paths in a single lowerdir= easily exceed that.
    std::string options;
    for (const auto &lower : lowers)
        options += "lowerdir+=" + lower.string() + ",";
    if (upper.empty()) {
        options.pop_back();
        s.run({"mkdir", "-p", merged.string()});
    } else {
        options += std::format("upperdir={},workdir={}", upper.string(), work.string());
        s.run({"mkdir", "-p", upper.string(), work.string(), merged.string()});
    }
    // an unprivileged overlay may only use the user xattr namespace
    s.run({"mount", "-t", "overlay", "overlay", "-o", options + ",userxattr", merged.string()});
}

void mount_overlay(Script &s, const std::vector<fs::path> &lowers, const fs::path &merged)
{
    mount_overlay(s, lowers, {}, {}, merged);
}

void strip_overlay_markers(Script &s, const fs::path &upper)
{
    // check with dirs have the marker, and then strip it from those
    s.line(
        std::format(
            R"sh(
getfattr --recursive --physical --no-dereference --absolute-names --match='^user\.overlay\.opaque$' --dump -- {} \
    | while IFS= read -r line; do
        case "$line" in
        "# file: "*) setfattr --no-dereference -x user.overlay.opaque -- "${{line#"# file: "}}" ;;
        esac
    done
)sh",
            shell_quote(upper.string())));
}

void copy_up(Script &s, const fs::path &merged, const std::vector<std::string> &paths)
{
    if (paths.empty())
        return;
    std::vector<std::string> files;
    for (const auto &path : paths)
        files.push_back((merged / path).string());
    // a rename inside the overlay leaves the copy in the upper layer
    s.line(
        std::format(
            R"sh(
for path in {}; do
    if [ -e "$path" ]; then
        cp -a "$path" "$path.mfb-copy"
        mv -f "$path.mfb-copy" "$path"
    fi
done
)sh",
            shell_join(files)));
}

void move_paths(
    Script &s,
    const fs::path &tree,
    std::string_view prefix,
    const std::vector<std::string> &patterns,
    const fs::path &destination)
{
    if (patterns.empty())
        return;
    // the globs stay unquoted, so the shell expands them
    auto globs = join(
        patterns | std::views::transform([&](const std::string &pattern) {
            return std::format("{}/{}", prefix, pattern.starts_with('/') ? pattern.substr(1) : pattern);
        }),
        " ");
    // the subshell keeps the shell options and the directory to itself
    s.line(
        std::format(
            R"sh(
(
    shopt -s globstar nullglob
    cd {0}
    for path in {1}; do
        [ -e "$path" ] || [ -L "$path" ] || continue
        mkdir -p {2}
        cp -a --parents "$path" {2}
        rm -rf "$path"
    done
)
)sh",
            shell_quote(tree.string()),
            globs,
            shell_quote(destination.string())));
}

void merge_into_usr(Script &s, const fs::path &tree)
{
    // a directory in an overlay layer would hide the link of the tree below
    s.line(
        std::format(
            R"sh(
for dir in bin sbin lib lib32 lib64 libx32; do
    if [ -d {0}/"$dir" ] && [ ! -L {0}/"$dir" ]; then
        mkdir -p {0}/usr/"$dir"
        cp -a {0}/"$dir"/. {0}/usr/"$dir"/
        rm -rf {0}/"$dir"
    fi
done
)sh",
            shell_quote(tree.string())));
}

void Sandbox::run(const Script &script, std::map<std::string, std::string> env)
{
    fs::create_directories(tmp_dir);
    auto report = tmp_dir / "failure-report";
    fs::remove(report);
    auto file = tmp_dir / std::format("script-{}.sh", ++m_scripts);
    auto text = std::format("exec {}>{}{}", REPORT_FD, shell_quote(report.string()), script.text());
    write_file(file, text);

    // The rootfs lives in the real work directory, which is visible through --dev-bind / /.
    auto command = in_namespace(
        {require_tool("bwrap").string(),
         // NOTE: bwrap must create the pid namespace itself, so the /proc
         // it mounts belongs to it. Behind "unshare --pid", it reuses the
         // host's procfs, which lacks the sandbox's pids, and the nested
         // bwrap of a part build can not find its own child.
         "--unshare-pid",
         "--dev-bind",
         "/",
         "/",
         "--proc",
         "/proc",
         "--unshare-ipc",
         "--unshare-uts",
         "--hostname",
         "manifold-builder",
         "--die-with-parent",
         "--",
         "bash",
         file.string()});
    command.env = std::move(env);
    command.env["TMPDIR"] = tmp_dir.string();

    LOG_DEBUG(logSandbox, "Running {} in the namespace:\n{}", file.string(), text);
    log::flush();
    int status = run_status(command);
    if (status != 0)
        fail("{}", describe_failure(script, fs::exists(report) ? read_file(report) : "", status));
}

void Sandbox::remove_tree(const fs::path &path) const
{
    if (!fs::is_directory(path))
        return;
    if (run_status(in_namespace({"rm", "-rf", "--one-file-system", path.string()})) != 0)
        LOG_WARNING(logSandbox, "Could not remove {}", path.string());
}

void Sandbox::check_available()
{
    require_tool("bwrap");
    if (run_status(in_namespace({"true"})) != 0)
        fail(
            "Could not create a user namespace with a subuid mapping! Check that unprivileged user namespaces "
            "are enabled and that /etc/subuid has an entry for your user");
}

void run_part_build(Script &s, const PartBuild &build)
{
    auto source_dir = fs::path("/run/build") / build.name;
    fs::create_directories(build.script_file.parent_path());
    write_file(build.script_file, traced_script(build.script));
    std::vector<std::string> argv = {
        "bwrap",
        "--bind",
        build.root.string(),
        "/",
        "--dev-bind",
        "/dev",
        "/dev",
        "--proc",
        "/proc",
        "--tmpfs",
        "/tmp",
        // create the mountpoints below /run on a tmpfs, the tree is read-only
        "--tmpfs",
        "/run",
        "--tmpfs",
        "/run/build",
        "--bind",
        build.sources.string(),
        source_dir.string(),
        "--dir",
        "/run/build/home",
        "--ro-bind",
        build.script_file.string(),
        "/run/script.sh",
    };
    for (const auto &[host, inside] : build.binds)
        argv.insert(argv.end(), {"--bind", host.string(), inside});
    argv.insert(
        argv.end(),
        {"--chdir",
         (build.subdir.empty() ? source_dir : source_dir / build.subdir).string(),
         "--clearenv",
         "--setenv",
         "HOME",
         "/run/build/home",
         "--setenv",
         "PATH",
         "/usr/bin:/bin",
         "--setenv",
         "LANG",
         "C.UTF-8",
         "--setenv",
         "SOURCE_DATE_EPOCH",
         std::to_string(build.epoch)});
    for (const auto &[name, value] : build.env)
        argv.insert(argv.end(), {"--setenv", name, value});
    argv.insert(argv.end(), {"--die-with-parent", "--", "bash", "-Eeuo", "pipefail", "/run/script.sh"});
    s.run(argv);
}

} // namespace mfb

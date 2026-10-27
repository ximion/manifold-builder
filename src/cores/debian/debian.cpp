/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cores/debian/debian.hpp"

#include "cores/debian/dpkg.hpp"
#include "error.hpp"
#include "manifest.hpp"
#include "process.hpp"
#include "sandbox.hpp"

#include <algorithm>
#include <manifold.h>
#include <set>

namespace mfb
{

namespace
{

/**
 * An architecture with its dpkg name, Manifold name and multiarch tuple.
 */
struct Architecture {
    std::string_view dpkg;
    std::string_view manifold;
    std::string_view multiarch;
};

constexpr Architecture ARCHITECTURES[] = {
    {"amd64", "x86_64", "x86_64-linux-gnu"},
    {"arm64", "aarch64", "aarch64-linux-gnu"},
    {"armel", "armel", "arm-linux-gnueabi"},
    {"armhf", "armhf", "arm-linux-gnueabihf"},
    {"i386", "i386", "i386-linux-gnu"},
    {"ppc64el", "powerpc64le", "powerpc64le-linux-gnu"},
    {"riscv64", "riscv64", "riscv64-linux-gnu"},
    {"s390x", "s390x", "s390x-linux-gnu"},
};

const Architecture *find_dpkg_architecture(std::string_view dpkg)
{
    auto found = std::ranges::find(ARCHITECTURES, dpkg, &Architecture::dpkg);
    return found == std::end(ARCHITECTURES) ? nullptr : found;
}

/**
 * Check that debootstrap has the EXCLUDE_DEPENDENCY patches.
 *
 * debuerreotype needs them for --merged-usr. We check this up front, so we
 * fail with a clear message instead of deep inside the bootstrap.
 */
void check_debootstrap()
{
    auto debootstrap = require_tool("debootstrap");
    const char *dir = std::getenv("DEBOOTSTRAP_DIR");
    fs::path functions = fs::path(dir ? dir : "/usr/share/debootstrap") / "functions";

    for (const auto &path : {debootstrap, functions}) {
        if (read_file(path).find("EXCLUDE_DEPENDENCY") == std::string::npos)
            fail(
                "{} lacks the EXCLUDE_DEPENDENCY patches that --merged-usr requires; see "
                "https://salsa.debian.org/installer-team/debootstrap/-/merge_requests/76",
                path.string());
    }
}

/**
 * Get the directory of the debuerreotype scripts.
 *
 * We read it from the environment instead of a command line option, to keep
 * the CLI free of distribution specifics.
 */
fs::path debuerreotype_scripts_dir()
{
    const char *dir = std::getenv("DEBUERREOTYPE_SCRIPT_DIR");
    return dir && *dir ? dir : "/usr/share/debuerreotype/scripts";
}

/**
 * Convert anything date(1) understands into a UNIX epoch.
 *
 * We call date(1) itself, so the accepted syntax matches debuerreotype-init
 * exactly.
 */
int64_t parse_timestamp(const std::string &timestamp)
{
    Command command{.argv = {"date", "--date", timestamp, "+%s"}, .quiet = true};
    std::optional<int64_t> epoch;
    try {
        epoch = parse_int(run_capture(command));
    } catch (const BuildError &) {
    }
    if (!epoch)
        fail("cannot parse snapshot timestamp '{}'", timestamp);
    return *epoch;
}

} // namespace

std::string manifold_architecture(const std::string &dpkg_architecture)
{
    const auto *found = find_dpkg_architecture(dpkg_architecture);
    if (found == nullptr)
        fail("no architecture known for dpkg architecture '{}'", dpkg_architecture);
    return std::string(found->manifold);
}

std::string dpkg_architecture(const std::string &manifold_architecture)
{
    auto found = std::ranges::find(ARCHITECTURES, manifold_architecture, &Architecture::manifold);
    if (found == std::end(ARCHITECTURES))
        fail("no dpkg architecture known for architecture '{}'", manifold_architecture);
    return std::string(found->dpkg);
}

DebianSystem::DebianSystem(fs::path scripts_dir, Bootstrap bootstrap, int64_t epoch)
    : m_scripts(std::move(scripts_dir)),
      m_bootstrap(std::move(bootstrap)),
      m_epoch(epoch)
{
    if (m_bootstrap.architecture.empty())
        m_bootstrap.architecture = dpkg_architecture(mf_get_host_architecture());
}

std::unique_ptr<DebianSystem> DebianSystem::for_core(const Bootstrap &bootstrap)
{
    return std::make_unique<DebianSystem>(debuerreotype_scripts_dir(), bootstrap, parse_timestamp(bootstrap.snapshot));
}

std::unique_ptr<DebianSystem> DebianSystem::for_layer(const Manifest &core, int64_t epoch)
{
    Bootstrap like_core;
    like_core.suite = core.build.suite.value_or("");
    like_core.architecture = dpkg_architecture(core.architecture);
    return std::make_unique<DebianSystem>(debuerreotype_scripts_dir(), like_core, epoch);
}

std::string DebianSystem::check_bootstrap(const Bootstrap &bootstrap)
{
    if (bootstrap.suite.empty())
        return "'bootstrap.suite' is required";
    if (bootstrap.snapshot.empty())
        return "'bootstrap.snapshot' is required: the build is pinned to that archive state";
    if (!bootstrap.architecture.empty() && find_dpkg_architecture(bootstrap.architecture) == nullptr)
        return std::format("'bootstrap.architecture' '{}' is not a dpkg architecture we know", bootstrap.architecture);
    return {};
}

std::string DebianSystem::script(std::string_view name) const
{
    return (m_scripts / name).string();
}

void DebianSystem::check_tools() const
{
    // every build runs commands in its trees with this one
    if (!fs::is_regular_file(m_scripts / "debuerreotype-chroot"))
        fail("debuerreotype scripts not found in {} (set DEBUERREOTYPE_SCRIPT_DIR)", m_scripts.string());
}

void DebianSystem::check_bootstrap_tools() const
{
    if (!fs::is_regular_file(m_scripts / "debuerreotype-init"))
        fail("debuerreotype-init not found in {} (set DEBUERREOTYPE_SCRIPT_DIR)", m_scripts.string());
    check_debootstrap();
}

std::string DebianSystem::architecture() const
{
    return manifold_architecture(m_bootstrap.architecture);
}

std::string DebianSystem::multiarch() const
{
    return std::string(find_dpkg_architecture(m_bootstrap.architecture)->multiarch);
}

int64_t DebianSystem::epoch() const
{
    return m_epoch;
}

std::string DebianSystem::package_system() const
{
    return "apt";
}

std::vector<std::string> DebianSystem::default_sdk_patterns() const
{
    // old-style debug packages, and the automatic ones of the debug archive
    return {"*-dev", "*-doc", "*-dbg", "*-dbgsym"};
}

std::vector<std::string> DebianSystem::locale_packages() const
{
    // locales generates the archive, libc-l10n has glibc's own translations
    return {"locales", "libc-l10n"};
}

void DebianSystem::describe_core(BuildInfo &build) const
{
    build.distribution = "debian";
    build.suite = m_bootstrap.suite;
    build.snapshot_url = tool_version({script(".snapshot-url.sh"), std::format("@{}", m_epoch)});
    build.tools["debuerreotype"] = tool_version({script("debuerreotype-version")});
    build.tools["debootstrap"] = tool_version({"debootstrap", "--version"});
}

void DebianSystem::bootstrap(Script &s, const fs::path &rootfs, const Recipe &recipe) const
{
    std::vector<std::string> init = {
        script("debuerreotype-init"), "--debian", "--merged-usr", "--arch", m_bootstrap.architecture};
    for (const auto &package : m_bootstrap.exclude)
        init.push_back("--exclude=" + package);
    init.insert(init.end(), {rootfs.string(), m_bootstrap.suite, std::format("@{}", m_epoch)});
    s.run(init);

    // use snapshot.debian.org for the whole build, so the timestamp pins
    // every package version
    s.run({script("debuerreotype-debian-sources-list"), "--deb822", "--snapshot", rootfs.string(), m_bootstrap.suite});
    s.run({script("debuerreotype-minimizing-config"), rootfs.string()});
    apt_get(s, rootfs, {"update", "-qq"});
    apt_get(s, rootfs, {"full-upgrade", "-yqq"});

    // slim down before anything else is installed, so dpkg keeps the
    // excluded files out of every package from here on
    if (m_bootstrap.slim)
        slimify(s, rootfs, recipe.locales);
}

void DebianSystem::refresh(Script &s, const fs::path &tree) const
{
    s.line(std::format("echo {} > {}", m_epoch, shell_quote((tree / "debuerreotype-epoch").string())));
    apt_get(s, tree, {"update", "-qq"});
}

void DebianSystem::dump_dependencies(
    Script &s,
    const fs::path &tree,
    const std::vector<std::string> &packages,
    const fs::path &output) const
{
    std::vector<std::string> depends = {
        script("debuerreotype-chroot"), tree.string(), "apt-cache", "depends", "--recurse", "--important"};
    depends.insert(depends.end(), packages.begin(), packages.end());
    s.line(std::format("{} > {}", shell_join(depends), shell_quote(output.string())));
}

DependencyGraph DebianSystem::read_dependencies(const fs::path &dump) const
{
    return parse_apt_cache_depends(read_file(dump));
}

void DebianSystem::install(Script &s, const fs::path &tree, const std::vector<std::string> &packages) const
{
    if (packages.empty())
        return;
    // NOTE: apt would download as _apt, which is a subuid outside the
    // namespace and would own partial/ in the shared cache. Root in the
    // namespace is our own uid.
    std::vector<std::string> install = {
        "-o",
        "Dir::Cache::Archives=/" + package_cache_dir(),
        "-o",
        "APT::Sandbox::User=root",
        "install",
        "-yqq",
        "--no-install-recommends",
        "--no-remove",
    };
    install.insert(install.end(), packages.begin(), packages.end());
    apt_get(s, tree, install);
}

void DebianSystem::apt_get(Script &s, const fs::path &tree, std::vector<std::string> arguments) const
{
    // We do what debuerreotype-apt-get does, minus its umask. The Release
    // files of a snapshot are long expired, so we don't check Valid-Until.
    arguments.insert(arguments.begin(), {"apt-get", "-o", "Acquire::Check-Valid-Until=false"});
    run_in_tree(s, tree, shell_join(arguments));
}

void DebianSystem::run_in_tree(Script &s, const fs::path &tree, const std::string &command) const
{
    // debuerreotype's scripts set umask 0002, which would leave
    // group-writable directories behind wherever a maintainer script or an
    // action creates one. Nothing may ask questions either, since nobody is
    // there to answer them.
    auto text = std::format(
        R"sh(
umask 022
export DEBIAN_FRONTEND=noninteractive
{}
)sh",
        command);
    s.run({script("debuerreotype-chroot"), tree.string(), "bash", "-Eeuo", "pipefail", "-c", text});
}

void DebianSystem::slimify(Script &s, const fs::path &rootfs, const std::vector<std::string> &locales) const
{
    // keep the catalogs below the language and below the full locale name,
    // e.g. /usr/share/locale/de and /usr/share/locale/de_DE for de_DE
    std::set<std::string> kept = {"locale.alias"};
    for (const auto &locale : locales) {
        kept.insert(locale);
        kept.insert(locale.substr(0, locale.find('_')));
    }

    // make dpkg leave these out of every package it unpacks from now on
    std::string config = R"(# Written by manifold-builder: this is a slim image.
path-exclude /usr/share/doc/*
path-include /usr/share/doc/*/copyright
path-exclude /usr/share/info/*
path-exclude /usr/share/man/*
path-exclude /usr/share/lintian/*
path-exclude /usr/share/locale/*
)";
    std::string keep_find;
    for (const auto &name : kept) {
        config += std::format("path-include /usr/share/locale/{}/*\n", name);
        keep_find += std::format(" ! -name {}", shell_quote(name));
    }
    s.write_file(rootfs / "etc/dpkg/dpkg.cfg.d/manifold-slim", config);
    // remove what the bootstrap already placed
    run_in_tree(
        s,
        rootfs,
        std::format(
            R"sh(
find /usr/share/doc -mindepth 2 \( -type f -o -type l \) ! -path '/usr/share/doc/*/copyright' -delete
find /usr/share/doc -mindepth 1 -type d -empty -delete
rm -rf /usr/share/info/* /usr/share/man/* /usr/share/lintian/*
find /usr/share/locale -mindepth 1 -maxdepth 1{} -exec rm -rf {{}} +
# some maintainer scripts expect the manual page directories
mkdir -p /usr/share/man/man{{1,2,3,4,5,6,7,8}}
)sh",
            keep_find));
}

void DebianSystem::generate_locales(Script &s, const fs::path &tree, const std::vector<std::string> &locales) const
{
    // We write /etc/locale.gen ourselves instead of preseeding debconf. The
    // debconf choice list spells some locales without the charset
    // (hi_IN UTF-8) and silently drops them if we use a different spelling.
    // locale-gen accepts any spelling.
    std::string list;
    for (const auto &locale : locales)
        list += std::format("{}.UTF-8 UTF-8\n", locale);
    run_in_tree(
        s,
        tree,
        std::format(
            R"sh(
printf '%s' {} > /etc/locale.gen
locale-gen
test "$(localedef --list-archive | wc -l)" -eq {} || {{ echo 'not every locale was generated' >&2; exit 1; }}
)sh",
            shell_quote(list),
            locales.size()));
}

void DebianSystem::finalize(Script &s, const fs::path &rootfs) const
{
    s.run({script("debuerreotype-debian-sources-list"), "--deb822", rootfs.string(), m_bootstrap.suite});
}

void DebianSystem::normalize(Script &s, const fs::path &tree) const
{
    // debuerreotype-fixup reads the epoch from a marker that only the
    // bootstrapped tree has
    s.line(std::format("echo {} > {}", m_epoch, shell_quote((tree / "debuerreotype-epoch").string())));
    s.run({script("debuerreotype-fixup"), tree.string()});
}

std::vector<std::string> DebianSystem::image_excludes() const
{
    return {
        "^debuerreotype-epoch$",
        "^var/cache/apt/.",
        "^var/lib/apt/lists/.",
        // contains host kernel information
        "^etc/apt/apt\\.conf\\.d/01autoremove-kernels$",
        // useful, but contain timestamps
        "^var/log/apt/(history|term)\\.log$",
        // fc-cache lists cache files in directory order, which varies
        "^var/log/fontconfig\\.log$",
    };
}

std::optional<InstalledPackages> DebianSystem::installed_packages(const fs::path &tree) const
{
    auto status = tree / "var/lib/dpkg/status";
    if (!fs::exists(status))
        return std::nullopt;
    return read_dpkg_status(status);
}

std::vector<std::string> DebianSystem::package_state_files() const
{
    return {
        "var/lib/dpkg/status",
        "var/lib/dpkg/diversions",
        "var/lib/dpkg/statoverride",
        "var/lib/dpkg/triggers/File",
        "var/lib/apt/extended_states",
    };
}

std::string DebianSystem::package_cache_dir() const
{
    // NOTE: Not apt's own archives directory: debuerreotype's docker-clean
    // configuration empties that one after every dpkg run.
    return "var/cache/apt/manifold-builder";
}

} // namespace mfb

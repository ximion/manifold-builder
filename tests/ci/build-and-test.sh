#!/bin/bash
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build manifold-builder, run its tests and install it to a staging directory,
# in a CI container (see Containerfile).
#
# Extra arguments for `meson setup` and `meson test` are taken from the
# CI_MESON_ARGS and CI_TEST_ARGS environment variables. The integration tests
# run if CI_INTEGRATION is "true".
#
set -euo pipefail

srcdir=$(cd "$(dirname "$0")/../.." && pwd)

if [ "$(id -u)" -eq 0 ]; then
    uid=$(stat -c %u "$srcdir")
    gid=$(stat -c %g "$srcdir")

    # with rootless podman, root of the container owns the tree, and the tests run as root
    if [ "$uid" -ne 0 ]; then
        getent group "$gid" > /dev/null || groupadd -g "$gid" ci
        getent passwd "$uid" > /dev/null || useradd -m -u "$uid" -g "$gid" ci
        user=$(getent passwd "$uid" | cut -d: -f1)

        # builds map these IDs into their user namespaces
        for map in /etc/subuid /etc/subgid; do
            grep -qs "^$user:" "$map" || echo "$user:100000:65536" >> "$map"
        done

        exec runuser -u "$user" -- "$0" "$@"
    fi
fi

# a new one, the source tree may have a build directory of its own
builddir="build-ci"
read -ra meson_args <<< "${CI_MESON_ARGS:-}"
read -ra test_args <<< "${CI_TEST_ARGS:-}"

echo "::group::Configure"
meson setup "$builddir" "$srcdir" \
    --buildtype=debugoptimized \
    -Dmanifold:docs=false \
    -Dmanifold:gir=disabled \
    "${meson_args[@]}"
echo "::endgroup::"

jobs=$(nproc)
echo "::group::Build"
meson compile -C "$builddir" -j "$((jobs < 12 ? jobs : 12))"
echo "::endgroup::"

# only run our own tests (skip subprojects)
echo "::group::Test"
meson test -C "$builddir" --print-errorlogs --suite Manifold_Builder:unit "${test_args[@]}"
echo "::endgroup::"

# a skipped test means lost coverage, e.g. when a tool is missing
if grep -E '^test cases: .* skipped' "$builddir/meson-logs/testlog.txt"; then
    echo "Tests were skipped" >&2
    exit 1
fi

if [ "${CI_INTEGRATION:-}" = true ]; then
    # integration tests
    echo "::group::Integration tests"
    meson test -C "$builddir" --print-errorlogs \
        --setup "Manifold Builder:integration" \
        --suite Manifold_Builder:integration \
        "${test_args[@]}"
    echo "::endgroup::"
fi

echo "::group::Install"
meson install -C "$builddir" --destdir install --skip-subprojects
echo "::endgroup::"

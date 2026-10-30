#!/bin/bash
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Install everything building and testing manifold-builder needs, on the
# distribution this runs on. Runs as root, when the CI container image is built.
#
set -euo pipefail

# shellcheck source=/dev/null
. /etc/os-release

case " $ID ${ID_LIKE:-} " in
    *" debian "*)
        export DEBIAN_FRONTEND=noninteractive
        install=(apt-get install -yq --no-install-recommends)
        packages=(
            build-essential
            meson
            ninja-build
            pkgconf
            cmake
            git
            ca-certificates
            python3
            python3-pytest
            libglib2.0-dev
            libblake3-dev
            libzip-dev
            libcatch2-dev
            # for the Manifold subproject
            systemd
            systemd-dev
            libsystemd-dev
            libcurl4-openssl-dev
            libcryptsetup-dev
            libelf-dev
            # for the AppStream subproject, if the distribution's version is too old
            gettext
            gperf
            itstool
            libcairo2-dev
            libfontconfig-dev
            libfreetype-dev
            libfyaml-dev
            libpango1.0-dev
            libvips-dev
            libxml2-dev
            libxmlb-dev
            # what builds run
            attr
            bubblewrap
            curl
            debootstrap
            debuerreotype
            erofs-utils
            erofsfuse
            fuse3
            openssh-client
            patch
            systemd-repart
            uidmap
            unzip
        )

        # Ubuntu's are too old or missing, it uses the subprojects
        if [ "$ID" = debian ]; then
            packages+=(libappstream-dev libappstream-compose-dev libglaze-dev quill-dev)
        fi

        apt-get update -q
        ;;

    *" fedora "*)
        install=(dnf install -y)
        # libraries are named by the pkg-config files they provide, which is the same everywhere
        packages=(
            gcc-c++
            meson
            ninja-build
            pkgconfig
            cmake
            git
            python3
            util-linux
            shadow-utils
            'pkgconfig(glib-2.0)'
            'pkgconfig(libblake3)'
            'pkgconfig(libzip)'
            catch-devel
            # for the Manifold subproject
            systemd
            'pkgconfig(libcurl)'
            'pkgconfig(libsystemd)'
            'pkgconfig(systemd)'
            'pkgconfig(udev)'
            'pkgconfig(libcryptsetup)'
            'pkgconfig(libelf)'
            # for the AppStream subproject, as the distribution's version is too old
            gettext
            gperf
            itstool
            'pkgconfig(cairo)'
            'pkgconfig(fontconfig)'
            'pkgconfig(freetype2)'
            'pkgconfig(libfyaml)'
            'pkgconfig(libxml-2.0)'
            'pkgconfig(pango)'
            'pkgconfig(vips)'
            'pkgconfig(xmlb)'
            # what builds run; Fedora has no debuerreotype, so no integration tests here yet
            attr
            bubblewrap
            curl
            erofs-utils
            openssh-clients
            patch
            tar
            unzip
        )
        ;;

    *" arch "*)
        install=(pacman -S --noconfirm --needed)
        packages=(
            base-devel
            meson
            cmake
            git
            python
            glib2
            glib2-devel
            appstream
            libblake3
            libzip
            glaze
            quill
            catch2
            # for the Manifold subproject
            systemd
            curl
            cryptsetup
            libelf
            # what builds run; Arch has no debuerreotype, so no integration tests here yet
            attr
            bubblewrap
            erofs-utils
            openssh
            patch
            unzip
        )

        pacman -Syu --noconfirm
        ;;

    *)
        echo "Unknown distribution: $ID" >&2
        exit 1
        ;;
esac

"${install[@]}" "${packages[@]}"

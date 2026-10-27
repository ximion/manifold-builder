# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build a small core from the Debian snapshot archive, then a platform and an
# application on top of it, found in a plain directory of extracted bundles.

import shutil
import zipfile
from pathlib import Path

import pytest
from conftest import DATA_DIR

CORE_RECIPE = '''\
type: core
component: com.example.testcore
version: "1.0"

bootstrap:
  suite: trixie
  snapshot: "2026-10-18T00:00:00Z"

locales: [de_DE]

packages:
  apt: [libzstd-dev]
packages-sdk:
  apt: [pkgconf]

actions:
  - name: marker
    run: echo testcore > /etc/testcore

parts:
  - name: data
    buildsystem: simple
    sources:
      - type: file
        path: parts/com.example.testcore.metainfo.xml
    build-commands:
      - install -D -m 0644 /dev/null $DESTDIR$MANIFOLD_DEST/share/testcore/data.txt
      - install -D -m 0644 /dev/null $DESTDIR$MANIFOLD_DEST/share/testcore/junk/junk.txt
      # an old-style path, which goes below /usr
      - install -D -m 0644 /dev/null $DESTDIR/lib/udev/rules.d/60-testcore.rules
      # the core's own MetaInfo, instead of one generated from the recipe
      - install -D -m 0644 com.example.testcore.metainfo.xml $DESTDIR/usr/local/share/metainfo/com.example.testcore.metainfo.xml
    cleanup:
      - /share/testcore/junk
    post-install:
      - ln -s data.txt /usr/share/testcore/link.txt
  # sees what the part before it installed, before cleanup, and can not write
  # to the tree
  - name: reader
    buildsystem: simple
    build-commands:
      - test -e /usr/share/testcore/junk/junk.txt
      - test -L /lib && test -e /lib/udev/rules.d/60-testcore.rules
      - "! touch /usr/share/testcore/stray 2>/dev/null"
      - install -D -m 0644 /usr/share/testcore/data.txt $DESTDIR$MANIFOLD_DEST/share/testcore/copy.txt

sdk-only:
  - include
  - lib/*/pkgconfig
'''

PLATFORM_RECIPE = '''\
type: platform
component: com.example.testcore.jpeg
version: "1.0"
name: JPEG Platform
summary: libjpeg on the test core
base: com.example.testcore
packages:
  apt: [libjpeg62-turbo-dev]
parts:
  - name: jpeg-data
    buildsystem: simple
    build-commands:
      # the platform's own development packages are there
      - test -e /usr/include/jpeglib.h
      - install -D -m 0644 /dev/null $DESTDIR$MANIFOLD_DEST/share/jpeg-platform/data.txt
      - install -D -m 0644 /dev/null $DESTDIR$MANIFOLD_DEST/include/jpeg-platform.h
sdk-only:
  - include
'''

APP_RECIPE = '''\
component: com.example.Hello
version: 1.0.0
command: hello
base: com.example.testcore.jpeg
base-version: "1.0"
parts:
  - name: metainfo
    buildsystem: simple
    sources:
      - type: file
        path: parts/com.example.Hello.metainfo.xml
    build-commands:
      - install -D -m 0644 com.example.Hello.metainfo.xml $MANIFOLD_DEST/share/metainfo/com.example.Hello.appdata.xml
  - name: hello
    buildsystem: simple
    build-commands:
      - install -D /usr/bin/true $MANIFOLD_DEST/bin/hello
'''

METAINFO_APP_RECIPE = '''\
component: com.example.Hello
version: 1.0.0
command: hello
base: com.example.testcore
base-version: "1.0"
metainfo-file: hello.metainfo.xml
generate-sdk: true
sdk-only:
  - share/hello/sdk-only.txt
parts:
  - parts/metainfo.yaml
  - name: hello
    buildsystem: simple
    build-commands:
      - install -D /usr/bin/true $MANIFOLD_DEST/bin/hello
      - install -D -m 0644 /dev/null $MANIFOLD_DEST/share/hello/sdk-only.txt
'''


def unpack(bundle: Path, runtimes: Path):
    runtimes.mkdir(exist_ok=True)
    with zipfile.ZipFile(bundle) as archive:
        archive.extractall(runtimes)


@pytest.fixture(scope='module')
def core(builder, tmp_path_factory):
    work = tmp_path_factory.mktemp('core')
    shutil.copytree(DATA_DIR / 'parts', work / 'parts')
    recipe = work / 'com.example.testcore.yaml'
    recipe.write_text(CORE_RECIPE)
    return builder.build(recipe, work / 'testcore.manifold')


def test_core(core):
    runtime = core.manifest('core')
    sdk = core.manifest('core', sdk=True)
    assert runtime['component'] == 'com.example.testcore'
    assert runtime['build']['distribution'] == 'debian'
    assert runtime['build']['suite'] == 'trixie'
    assert sdk['base']['content-hash'] == runtime['content-hash']

    # the development package goes to the SDK, its library to the runtime
    apt = runtime['packages']['apt']
    assert 'libzstd1' in apt and 'libzstd-dev' not in apt
    assert 'libzstd-dev' in sdk['packages']['apt']
    assert 'pkgconf' in sdk['packages']['apt']
    # locales pull in the packages that generate them
    assert 'locales' in apt and 'libc-l10n' in apt

    tree = core.extract(runtime)
    assert (tree / 'manifold.json').is_file()
    # the MetaInfo the core ships moves to /usr/share unchanged, only the catalog names the image
    metainfo = (tree / 'usr/share/metainfo/com.example.testcore.metainfo.xml').read_text()
    assert metainfo == (DATA_DIR / 'parts/com.example.testcore.metainfo.xml').read_text()
    assert not (tree / 'usr/local/share/metainfo/com.example.testcore.metainfo.xml').exists()
    assert '<name>Test Core</name>' in core.component(runtime)
    assert not [name for name in core.names if name.endswith('.metainfo.xml')]
    assert (tree / 'etc/testcore').read_text() == 'testcore\n'
    assert (tree / 'etc/ld.so.conf.d/manifold-app.conf').is_file()
    assert (tree / 'etc/fonts/conf.d/50-manifold-host-fonts.conf').is_file()
    assert (tree / 'usr/share/testcore/data.txt').is_file()
    assert (tree / 'usr/share/testcore/link.txt').is_symlink()
    assert (tree / 'usr/share/testcore/copy.txt').is_file()
    assert (tree / 'lib').is_symlink()
    assert (tree / 'usr/lib/udev/rules.d/60-testcore.rules').is_file()
    assert not (tree / 'usr/share/testcore/junk').exists()
    assert not (tree / 'usr/include').exists()
    # documentation always goes to the SDK
    assert not (tree / 'usr/share/doc').exists()
    # passwd installs chage setgid shadow, which images do not keep
    assert (tree / 'usr/bin/chage').is_file()
    assert not [
        path for path in tree.rglob('*') if not path.is_symlink() and path.stat().st_mode & 0o6000
    ]

    sdk_tree = core.extract(sdk)
    assert (sdk_tree / 'manifold.json').is_file()
    # the SDK's MetaInfo is generated from the core's
    sdk_metainfo = (
        sdk_tree / 'usr/share/metainfo/com.example.testcore.sdk.metainfo.xml'
    ).read_text()
    assert '<name>Test Core (SDK)</name>' in sdk_metainfo
    assert '<extends>com.example.testcore</extends>' in sdk_metainfo
    assert 'manifold.id' not in sdk_metainfo
    assert '<name>Test Core (SDK)</name>' in core.component(sdk)
    assert (sdk_tree / 'usr/include/zstd.h').is_file()


def test_core_is_reproducible(builder, core, tmp_path):
    shutil.copytree(DATA_DIR / 'parts', tmp_path / 'parts')
    recipe = tmp_path / 'com.example.testcore.yaml'
    recipe.write_text(CORE_RECIPE)
    again = builder.build(recipe, tmp_path / 'testcore.manifold')
    for sdk in (False, True):
        first = core.manifest('core', sdk)['image']
        second = again.manifest('core', sdk)['image']
        assert first['verity-root-hash'] == second['verity-root-hash']
        assert first['sha256'] == second['sha256']


def test_platform_and_app(builder, core, tmp_path):
    runtimes = tmp_path / 'runtimes'
    unpack(core.path, runtimes)

    recipe = tmp_path / 'com.example.testcore.jpeg.yaml'
    recipe.write_text(PLATFORM_RECIPE)
    platform = builder.build(recipe, tmp_path / 'jpeg.manifold', '--runtimes-dir', str(runtimes))
    runtime = platform.manifest('platform')
    assert runtime['base']['component'] == 'com.example.testcore'
    assert 'libjpeg62-turbo' in runtime['packages']['apt']
    assert 'libjpeg62-turbo-dev' in platform.manifest('platform', sdk=True)['packages']['apt']
    # the platform ships no MetaInfo, so it is generated from the recipe
    metainfo = (
        platform.extract(runtime) / 'usr/share/metainfo/com.example.testcore.jpeg.metainfo.xml'
    ).read_text()
    assert '<name>JPEG Platform</name>' in metainfo
    assert 'manifold.id' not in metainfo
    # the part goes to the platform, its header to the platform's SDK
    assert (platform.extract(runtime) / 'usr/share/jpeg-platform/data.txt').is_file()
    assert not (platform.extract(runtime) / 'usr/include').exists()
    sdk = platform.manifest('platform', sdk=True)
    sdk_tree = platform.extract(sdk)
    assert (sdk_tree / 'usr/include/jpeg-platform.h').is_file()
    # the SDK's package database also has what the core SDK below it installed
    assert {'libzstd-dev', 'pkgconf'} <= sdk['packages']['apt'].keys()
    # the SDK installs the library too, but only the platform stores it
    assert list(platform.extract(runtime).glob('usr/lib/*/libjpeg.so.62*'))
    assert not list(sdk_tree.glob('usr/lib/*/libjpeg.so.62*'))

    unpack(platform.path, runtimes)
    shutil.copytree(DATA_DIR / 'parts', tmp_path / 'parts')
    recipe = tmp_path / 'com.example.Hello.yaml'
    recipe.write_text(APP_RECIPE)
    app = builder.build(recipe, tmp_path / 'hello.manifold', '--runtimes-dir', str(runtimes))
    kit = app.manifest('kit')
    assert kit['component'].startswith('com.example.testcore.jpeg.')
    assert kit['base']['component'] == 'com.example.testcore.jpeg'
    tree = app.extract(app.manifest('app'))
    assert (tree / 'app/bin/hello').is_file()
    # the app's appdata file is renamed, and its content kept
    metainfo = (tree / 'app/share/metainfo/com.example.Hello.metainfo.xml').read_text()
    assert metainfo == (DATA_DIR / 'parts/com.example.Hello.metainfo.xml').read_text()
    assert not (tree / 'app/share/metainfo/com.example.Hello.appdata.xml').exists()
    assert '<name>Hello</name>' in app.component(app.manifest('app'))


def test_app_metainfo_file(builder, core, tmp_path):
    runtimes = tmp_path / 'runtimes'
    unpack(core.path, runtimes)
    shutil.copytree(DATA_DIR / 'parts', tmp_path / 'parts')
    # the recipe's file replaces the one the app installs
    shipped = (DATA_DIR / 'parts/com.example.Hello.metainfo.xml').read_text()
    (tmp_path / 'hello.metainfo.xml').write_text(
        shipped.replace('Says hello', 'Says hello from the recipe')
    )
    recipe = tmp_path / 'com.example.Hello.yaml'
    recipe.write_text(METAINFO_APP_RECIPE)
    bundle = builder.build(recipe, tmp_path / 'hello.manifold', '--runtimes-dir', str(runtimes))

    app = bundle.manifest('app')
    metainfo = (
        bundle.extract(app) / 'app/share/metainfo/com.example.Hello.metainfo.xml'
    ).read_text()
    assert metainfo == (tmp_path / 'hello.metainfo.xml').read_text()
    assert 'Says hello from the recipe' in bundle.component(app)

    # the app SDK sits on the kit SDK and keeps the sdk-only files
    app_sdk = bundle.manifest('app', sdk=True)
    kit_sdk = bundle.manifest('kit', sdk=True)
    assert app_sdk['component'] == 'com.example.Hello.sdk'
    assert app_sdk['base']['component'] == kit_sdk['component']
    # the kit SDK installs nothing, but keeps the package database of the core SDK
    assert 'libzstd-dev' in kit_sdk['packages']['apt']
    # and the app SDK lists what it builds against
    assert app_sdk['packages'] == kit_sdk['packages']
    sdk_tree = bundle.extract(app_sdk)
    assert (sdk_tree / 'app/share/hello/sdk-only.txt').is_file()
    # the app SDK's MetaInfo is generated from the app's
    sdk_metainfo = (sdk_tree / 'app/share/metainfo/com.example.Hello.sdk.metainfo.xml').read_text()
    assert '<name>Hello (SDK)</name>' in sdk_metainfo
    # but it belongs to the app
    assert '<extends>com.example.Hello</extends>' in sdk_metainfo

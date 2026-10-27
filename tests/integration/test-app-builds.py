# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Application builds on the net.debian.core13 installed on the host.

import hashlib
import shutil
import tarfile
from pathlib import Path

import pytest
from conftest import CORE, DATA_DIR

pytestmark = pytest.mark.usefixtures('core_installed')

HELLO_RECIPE = '''\
component: com.example.Hello
version: 1.0.0
command: hello-meson

base: {core}
base-version: "13"

packages-sdk:
  apt: [autoconf, automake]

build-options:
  cflags: -DMANIFOLD_CFLAGS_OK
  env:
    HELLO_FROM_ENV: from-env

sdk-only:
  - share/hello/sdk-only.txt

parts:
  - parts/metainfo.yaml
  - name: meson
    buildsystem: meson
    sources:
      - type: dir
        path: hello
      - type: patch
        path: greeting.patch
    post-install:
      - ln -s hello-meson $MANIFOLD_DEST/bin/hello
    parts:
      - name: cmake
        buildsystem: cmake
        sources:
          - type: archive
            path: hello-1.0.tar.gz
            sha256: {archive_sha256}
      - name: cmake-make
        buildsystem: cmake-make
        config-opts: [-DHELLO_NAME=hello-cmake-make]
        sources:
          - type: dir
            path: hello
      - name: autotools
        buildsystem: autotools
        sources:
          - type: dir
            path: hello
      - name: library
        buildsystem: simple
        build-commands:
          - printf 'int answer(void) {{ return 42; }}\\n' > answer.c
          - cc -shared -fPIC -Wl,-soname,libanswer.so.1 -o libanswer.so.1 answer.c
          - printf 'int answer(void);\\nint main(void) {{ return answer() != 42; }}\\n' > main.c
          - cc -o answer main.c ./libanswer.so.1
          - install -D -m 0644 libanswer.so.1 $MANIFOLD_DEST/lib/libanswer.so.1
          - install -D answer $MANIFOLD_DEST/bin/answer
        cleanup: [/bin/answer, /lib/libanswer.so.1]
      - name: simple
        buildsystem: simple
        subdir: src
        sources:
          - type: dir
            path: hello
            dest: src
        build-commands:
          # the parts before this one are visible below the prefix
          - test -x $MANIFOLD_DEST/bin/hello-cmake
          # and so are the libraries they installed
          - answer
          - cc $CFLAGS -o hello-simple hello.c
          - install -D hello-simple $MANIFOLD_DEST/bin/hello-simple
          - mkdir -p $MANIFOLD_DEST/share/hello/junk
          - echo "$HELLO_FROM_ENV $MANIFOLD_ID $MANIFOLD_ARCH $MANIFOLD_BUILDER_N_JOBS" > $MANIFOLD_DEST/share/hello/env.txt
          - touch $MANIFOLD_DEST/share/hello/junk/garbage.txt $MANIFOLD_DEST/share/hello/sdk-only.txt
        cleanup:
          - /share/hello/junk
      - name: never
        buildsystem: simple
        only-arches: [s390x]
        build-commands: ['false']
'''

MFTESTAPP_RECIPE = '''\
component: com.example.mftestapp
version: 0.1.0
command: mftestapp

base: {core}
base-version: "13"

packages-sdk:
  apt: [qt6-base-dev]

parts:
  - name: mftestapp
    buildsystem: meson
    sources:
      - type: git
        url: https://github.com/mfhq/mf-test-app.git
        branch: main
'''


@pytest.fixture
def hello_recipe(tmp_path: Path) -> Path:
    shutil.copytree(DATA_DIR, tmp_path, dirs_exist_ok=True)
    archive = tmp_path / 'hello-1.0.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(tmp_path / 'hello', arcname='hello-1.0')
    sha256 = hashlib.sha256(archive.read_bytes()).hexdigest()
    recipe = tmp_path / 'com.example.Hello.yaml'
    recipe.write_text(HELLO_RECIPE.format(core=CORE, archive_sha256=sha256))
    return recipe


def test_every_build_system(builder, hello_recipe, tmp_path):
    bundle = builder.build(hello_recipe, tmp_path / 'hello.manifold', '--jobs', '3')

    kit = bundle.manifest('kit')
    assert kit['component'].startswith(CORE + '.')
    # without generate-sdk, neither the app SDK nor the kit SDK ships
    assert [m['kind'] for m in bundle.manifests if m['sdk']] == []

    app = bundle.manifest('app')
    assert app['component'] == 'com.example.Hello'
    assert app['app']['command'] == 'hello-meson'
    assert app['base']['component'] == kit['component']
    types = {(s['part'], s['type']) for s in app['sources']}
    assert ('metainfo', 'file') in types
    assert ('meson', 'patch') in types
    assert ('cmake', 'archive') in types
    assert all(s.get('commit') or s.get('sha256') or s.get('tree-hash') for s in app['sources'])
    assert not any(s['part'] == 'never' for s in app['sources'])

    tree = bundle.extract(app) / 'app'
    assert (tree / 'manifold.json').is_file()
    assert (tree / 'share/metainfo/com.example.Hello.metainfo.xml').is_file()
    for program in [
        'hello-meson',
        'hello-cmake',
        'hello-cmake-make',
        'hello-autotools',
        'hello-simple',
    ]:
        binary = (tree / 'bin' / program).read_bytes()
        assert binary.startswith(b'\x7fELF'), program
        # build-options reach every build system
        assert b'manifold-cflags-ok' in binary, program
        # only the meson part has the patch
        patched = b'greeting: patched' in binary
        assert patched == (program == 'hello-meson'), program
    assert (tree / 'bin/hello').is_symlink()
    assert (tree / 'share/hello/env.txt').read_text() == 'from-env com.example.Hello x86_64 3\n'
    # cleanup removed the junk directory with its contents
    assert not (tree / 'share/hello/junk').exists()
    # without an application SDK, sdk-only paths are dropped
    assert not (tree / 'share/hello/sdk-only.txt').exists()


def test_mftestapp(builder, tmp_path):
    recipe = tmp_path / 'com.example.mftestapp.yaml'
    recipe.write_text(MFTESTAPP_RECIPE.format(core=CORE))
    bundle = builder.build(recipe, tmp_path / 'mftestapp.manifold')

    # the kit has what the app links against, found by the scan of /app
    kit = bundle.manifest('kit')
    assert 'libqt6core6t64' in kit['packages']['apt']
    assert not any(name.endswith('-dev') for name in kit['packages']['apt'])

    app = bundle.manifest('app')
    assert app['sources'][0]['url'] == 'https://github.com/mfhq/mf-test-app.git'
    assert len(app['sources'][0]['commit']) == 40
    tree = bundle.extract(app) / 'app'
    assert (tree / 'bin/mftestapp').is_file()
    assert (tree / 'share/applications/com.example.mftestapp.desktop').is_file()
    # the catalog has the app's icon in every size, rendered from its SVG
    component = bundle.component(app)
    assert 'type="desktop-application"' in component
    assert 'com.example.mftestapp.jxl' in component
    for size in ('48x48', '64x64', '128x128', '64x64@2'):
        assert f'catalog/icons/{size}/com.example.mftestapp.jxl' in bundle.names


FAILING_RECIPE = '''\
component: com.example.Failing
version: 1.0.0
command: failing

base: {core}
base-version: "13"

parts:
  - name: broken
    buildsystem: simple
    build-commands:
      - echo hello
      - test -e /nonexistent
      - echo never
'''


def test_failing_command_is_named(builder, tmp_path):
    recipe = tmp_path / 'com.example.Failing.yaml'
    recipe.write_text(FAILING_RECIPE.format(core=CORE))
    result = builder.run(
        'build', '--output', str(tmp_path / 'failing.manifold'), str(recipe), cwd=tmp_path
    )
    assert result.returncode == 1
    assert "part 'broken': Command failed with status 1: test -e /nonexistent" in result.stdout
    # the recipe's commands are shown as they run, our own are not
    assert '+ echo hello\nhello\n' in result.stdout
    assert 'never' not in result.stdout
    assert 'trap' not in result.stdout and 'mfb_step' not in result.stdout


def test_malformed_source_date_epoch(builder, hello_recipe, tmp_path, monkeypatch):
    monkeypatch.setenv('SOURCE_DATE_EPOCH', '123abc')
    result = builder.run(
        'build',
        '--output',
        str(tmp_path / 'hello.manifold'),
        str(hello_recipe),
        cwd=hello_recipe.parent,
    )
    assert result.returncode == 1
    assert "SOURCE_DATE_EPOCH must be a number of seconds since 1970, not '123abc'" in result.stdout


BROKEN_METAINFO = '''\
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>com.example.Broken</id>
  <name>Broken</name>
  <summary>Has no description</summary>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>MIT</project_license>
</component>
'''


def test_invalid_metainfo_file(builder, tmp_path):
    (tmp_path / 'broken.metainfo.xml').write_text(BROKEN_METAINFO)
    recipe = tmp_path / 'com.example.Broken.yaml'
    recipe.write_text(
        f'component: com.example.Broken\nversion: 1\ncommand: broken\nbase: {CORE}\nbase-version: "13"\n'
        'metainfo-file: broken.metainfo.xml\n'
        'parts: [{name: p, buildsystem: simple, build-commands: ["true"]}]\n'
    )
    result = builder.run(
        'build', '--output', str(tmp_path / 'broken.manifold'), str(recipe), cwd=tmp_path
    )
    # an error in the recipe's MetaInfo fails the build before anything is built
    assert result.returncode == 1
    assert 'is not valid MetaInfo: app-description-required' in result.stdout
    assert 'Building part' not in result.stdout

# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Fixtures for running the real manifold-builder binary. The builds need network access,
# and the core net.debian.core13 installed in the host's Manifold store.

import json
import os
import subprocess
import zipfile
from pathlib import Path
from xml.etree import ElementTree

import pytest

STORE = Path('/var/lib/manifold')
CORE = 'net.debian.core13'
DATA_DIR = Path(__file__).parent / 'data'


class Bundle:
    """A built .manifold bundle, with access to its manifests and image trees."""

    def __init__(self, path: Path, work_dir: Path):
        self.path = path
        self.work_dir = work_dir
        with zipfile.ZipFile(path) as archive:
            self.names = archive.namelist()
            self.manifests = [
                json.loads(archive.read(name))
                for name in self.names
                if name.endswith('.manifold.json')
            ]

    def manifest(self, kind: str, sdk: bool = False) -> dict:
        found = [m for m in self.manifests if m['kind'] == kind and m['sdk'] == sdk]
        assert len(found) == 1, f'expected one {kind} image (sdk={sdk}) in {self.names}'
        return found[0]

    def extract(self, manifest: dict) -> Path:
        """Extract the image a manifest describes and return its tree."""
        image = manifest['image']
        target = self.work_dir / f'{manifest["component"]}.tree'
        if target.exists():
            return target
        with zipfile.ZipFile(self.path) as archive:
            raw = Path(archive.extract(image['file'], self.work_dir))
        subprocess.run(
            [
                'fsck.erofs',
                f'--offset={image["data-offset"]}',
                f'--extract={target}',
                '--no-preserve',
                str(raw),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        raw.unlink()
        return target

    def component(self, manifest: dict) -> str:
        """Get the catalog component of the image a manifest describes, as XML."""
        ref = f'{manifest["architecture"]}/{manifest["component"]}/{manifest["version"]}'
        with zipfile.ZipFile(self.path) as archive:
            catalog = ElementTree.fromstring(archive.read('catalog/manifold-local.xml'))
        for component in catalog.iter('component'):
            if component.findtext("bundle[@type='manifold']") == ref:
                return ElementTree.tostring(component, encoding='unicode')
        pytest.fail(f'the catalog does not describe {ref}')


class Builder:
    def __init__(self, path: Path):
        self.path = path

    def run(self, *args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(self.path), *args],
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    def build(self, recipe: Path, output: Path, *args: str) -> Bundle:
        result = self.run('build', '--output', str(output), *args, str(recipe), cwd=recipe.parent)
        if result.returncode != 0:
            pytest.fail(f'build of {recipe.name} failed:\n{result.stdout[-30000:]}')
        check = self.run('check', str(output))
        assert check.returncode == 0, check.stdout
        extract_dir = output.parent / (output.name + '.extracted')
        extract_dir.mkdir(exist_ok=True)
        return Bundle(output, extract_dir)

    def build_fails(self, recipe: Path, output: Path, *args: str) -> str:
        """Build a recipe that must fail, and return what the builder printed."""
        result = self.run('build', '--output', str(output), *args, str(recipe), cwd=recipe.parent)
        assert result.returncode == 1, result.stdout
        return result.stdout


@pytest.fixture(scope='session', autouse=True)
def cache_dir(tmp_path_factory) -> Path:
    """Keep the builds' downloads and packages out of the user's own cache."""
    path = tmp_path_factory.mktemp('cache')
    os.environ['XDG_CACHE_HOME'] = str(path)
    return path


@pytest.fixture(scope='session')
def builder() -> Builder:
    path = os.environ.get('MANIFOLD_BUILDER')
    if not path:
        pytest.fail(
            'MANIFOLD_BUILDER must name the manifold-builder binary; run the tests through meson'
        )
    return Builder(Path(path))


@pytest.fixture(scope='session')
def core_installed():
    if not (STORE / 'runtimes' / CORE / 'current' / 'manifest.json').exists():
        pytest.skip(f'{CORE} is not installed in {STORE}')

# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Tests for the command line that build nothing.


def test_version(builder):
    result = builder.run('--version')
    assert result.returncode == 0
    assert result.stdout.startswith('manifold-builder ')


def test_usage(builder):
    assert builder.run().returncode == 2
    assert builder.run('build').returncode == 2


def test_invalid_recipe(builder, tmp_path):
    recipe = tmp_path / 'broken.yaml'
    recipe.write_text(
        'component: com.example.Broken\nversion: 1\ncommand: broken\nbase: net.debian.core13\n'
        'base-version: "13"\nparts: [{name: p, buildsystem: quake}]\n'
    )
    output = builder.build_fails(recipe, tmp_path / 'broken.manifold')
    assert "'buildsystem' must be" in output


def test_check_refuses_garbage(builder, tmp_path):
    bundle = tmp_path / 'garbage.manifold'
    bundle.write_bytes(b'not a zip file')
    assert builder.run('check', str(bundle)).returncode == 1

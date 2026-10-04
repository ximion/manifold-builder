#
# Tests
#

test_shared_src = [
    'testshared.hpp',
    'testshared.cpp',
]

test_builder = executable('test-builder',
    test_shared_src,
    'test-bundle.cpp',
    'test-buildsystems.cpp',
    'test-debian.cpp',
    'test-manifest.cpp',
    'test-recipe.cpp',
    'test-sandbox.cpp',
    'test-sources.cpp',
    'test-store.cpp',
    'test-utils.cpp',
    dependencies : [
        mfbuilder_dep,
        zip_dep,
        catch2_dep
    ],
    cpp_args : ['-DMFB_TESTS_DIR="@0@"'.format(meson.current_source_dir())],
)

test('builder', test_builder,
    suite : 'unit',
)

# Full builds with the real binary. They need network access, user namespaces
# and an installed net.debian.core13, so we skip them by default.
# Run them with "meson test --setup integration".
python = import('python').find_installation('python3', modules : ['pytest'], required : false)
if python.found()
    test('integration', python,
        args : [
            '-m', 'pytest',
            '-p', 'no:cacheprovider',
            '-v',
            '--basetemp', meson.current_build_dir() / 'integration-tmp',
            meson.current_source_dir() / 'integration',
        ],
        env : {
            'MANIFOLD_BUILDER' : mfbuilder_exe.full_path(),
            'PYTHONDONTWRITEBYTECODE' : '1',
        },
        depends : mfbuilder_exe,
        suite : 'integration',
        timeout : 0,
    )
endif

add_test_setup('default', exclude_suites : ['integration'], is_default : true)
add_test_setup('integration')

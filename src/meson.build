
mfb_lib_sources = files(
    'app-build.cpp',
    'appstream.cpp',
    'base-store.cpp',
    'build.cpp',
    'buildsystems/autotools.cpp',
    'buildsystems/buildsystem.cpp',
    'buildsystems/cmake.cpp',
    'buildsystems/meson.cpp',
    'buildsystems/simple.cpp',
    'bundle.cpp',
    'checksum.cpp',
    'core-build.cpp',
    'cores/core-system.cpp',
    'cores/debian/debian.cpp',
    'cores/debian/dpkg.cpp',
    'identity.cpp',
    'manifest.cpp',
    'image.cpp',
    'logging.cpp',
    'recipe.cpp',
    'process.cpp',
    'sandbox.cpp',
    'sources/archive.cpp',
    'sources/dir.cpp',
    'sources/file.cpp',
    'sources/git.cpp',
    'sources/patch.cpp',
    'sources/source.cpp',
    'utils.cpp',
)

mfbuilder_lib = static_library('mfbuilder',
    mfb_lib_sources,
    dependencies : [quill_dep, blake3_dep, glaze_dep, appstream_dep, ascompose_dep, glib_dep, manifold_dep],
)

mfbuilder_dep = declare_dependency(
    link_with : mfbuilder_lib,
    include_directories : include_directories('.'),
    dependencies : [
        glaze_dep,
        glib_dep,
        manifold_dep,
        quill_dep
    ],
)

mfbuilder_exe = executable('manifold-builder',
    'main.cpp',
    dependencies : [
        mfbuilder_dep,
        glib_dep
    ],
    install : true,
)

project('Manifold Builder', 'cpp',
    meson_version : '>=1.4',
    default_options : ['c_std=c23', 'cpp_std=c++23', 'warning_level=2'],
    license : 'LGPL-2.1-or-later',
    version : '0.1.0'
)

mfb_version = meson.project_version()

source_root = meson.project_source_root()
build_root = meson.project_build_root()
cxx = meson.get_compiler('cpp')

fs = import('fs')

add_project_arguments(
    '-DMFB_VERSION="@0@"'.format(mfb_version),
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
    language : 'cpp'
)

#
# Dependencies
#
appstream_dep = dependency('appstream', version: '>= 1.2.2', default_options: [
    'bash-completion=false',
    'compose=true',
    'display-detection=none',
    'gir=false',
    'man=false',
    'docs=false',
    'stemming=false',
    'tools=false',
])
ascompose_dep = dependency('appstream-compose', version: '>= 1.2.2')
quill_dep     = dependency('quill', version: '>= 11.1', fallback: ['quill', 'quill_dep']).as_system()
blake3_dep    = dependency('libblake3', required: true)
zip_dep       = dependency('libzip', required: true)
glaze_dep     = dependency('glaze', version: '>= 9.0', method: 'cmake', modules: ['glaze::glaze'])
glib_dep      = dependency('glib-2.0', version: '>= 2.74')
manifold_dep  = dependency('manifold', version: '>= 0.0.1', fallback: ['manifold', 'manifold_dep'])

catch2_dep    = dependency('catch2-with-main', version : '>= 3.0')

#
# Subdirs
#
subdir('src')
subdir('tests')

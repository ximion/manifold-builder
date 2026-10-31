#
# Data
#

# AppArmor profile
apparmor_parser_exe = find_program('apparmor_parser',
                               dirs: ['/usr/sbin', '/sbin'],
                               version: '>= 4.0',
                               required: get_option('apparmor'),
)
if apparmor_parser_exe.found()
    apparmor_conf = configuration_data()
    apparmor_conf.set('MFB_BINDIR', get_option('prefix') / get_option('bindir'))

    configure_file(
        input: 'manifold-builder.apparmor.in',
        output: 'manifold-builder',
        configuration: apparmor_conf,
        install: true,
        install_dir: get_option('apparmor_dir'),
    )

    # load it right away, it would only be loaded on the next boot otherwise
    meson.add_install_script(apparmor_parser_exe,
        '--replace',
        get_option('apparmor_dir') / 'manifold-builder',
        skip_if_destdir: true,
    )
endif

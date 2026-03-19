project('gmp', 'c',
        version: run_command('python', 'scripts/gitversion.py',
                             check: true).stdout().strip(),
        default_options: ['c_std=c17'])
py = import('python').find_installation(pure: false)
libzz = dependency('zz', version: '>= 0.7.0')
py.extension_module('gmp', ['fmt.c', 'gmp.c', 'utils.c'],
                    install: true, dependencies: libzz)
install_dir = py.get_install_dir()

# Generate version.py for sdist
meson.add_dist_script(['scripts/gitversion.py', '--meson-dist',
                       '--write', 'version.py'])
fs = import('fs')
if not fs.exists('version.py')
  generate_version = custom_target('generate-version',
                                   install: true,
                                   build_always_stale: true,
                                   build_by_default: true,
                                   output: 'version.py',
                                   input: 'scripts/gitversion.py',
                                   command: [py, '@INPUT@', '--write',
                                             '@OUTPUT@'],
                                   install_dir: install_dir,
                                   install_tag: 'python-runtime')
endif

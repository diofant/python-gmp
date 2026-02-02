project('gmp', 'c',
        version: run_command('python', 'scripts/gitversion.py',
                             check: true).stdout().strip(),
        default_options: ['c_std=c17'])
py = import('python').find_installation(pure: false)
libzz = dependency('zz', version: '>= 0.7.0')
py.extension_module('gmp', ['fmt.c', 'gmp.c', 'utils.c'],
                    install: true, dependencies: libzz)

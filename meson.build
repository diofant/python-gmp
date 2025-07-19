project('gmp', 'c',
        version: run_command('python', '-m', 'setuptools_scm',
                             check: true).stdout().strip())
py = import('python').find_installation(pure: false)
libgmp = dependency('gmp', version: '>= 6.2.0')
py.extension_module('gmp', ['fmt.c', 'main.c', 'zz.c'],
                    dependencies: libgmp, install: true)

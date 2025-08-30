project('gmp', 'c',
        version: run_command('python', '-m', 'setuptools_scm',
                             check: true).stdout().strip())
py = import('python').find_installation(pure: false)
libgmp = dependency('gmp', version: '>= 6.3.0')
if build_machine.system() != 'windows'
  libzz = shared_library('zz', 'libzz/zz.c', install: true,
                         dependencies: libgmp,
                         install_dir: py.get_install_dir())
  py.extension_module('gmp', ['fmt.c', 'gmp.c', 'utils.c'],
                      install: true, link_with: libzz,
                      install_rpath: '$ORIGIN')
else
  libzz = static_library('zz', 'libzz/zz.c', install: false,
                         dependencies: libgmp)
  py.extension_module('gmp', ['fmt.c', 'gmp.c', 'utils.c'],
                      install: true, link_with: libzz)
endif

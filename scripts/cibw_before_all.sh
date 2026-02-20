#!/bin/bash

set -e -x

GMP_VERSION=6.3.0

PREFIX="$(pwd)/.local/"

# -- build GMP --
curl -s -O https://ftp.gnu.org/gnu/gmp/gmp-${GMP_VERSION}.tar.xz
tar -xf gmp-${GMP_VERSION}.tar.xz
cd gmp-${GMP_VERSION}
patch -N -Z -p1 < ../scripts/fat_build_fix.diff
if [ "$OSTYPE" = "msys" ] || [ "$OSTYPE" = "cygwin" ]
then
  patch -N -Z -p0 < ../scripts/dll-importexport.diff
fi

unset CFLAGS

# config.guess uses microarchitecture and configfsf.guess doesn't
# We replace config.guess with configfsf.guess to avoid microarchitecture
# specific code in common code.
rm config.guess && mv configfsf.guess config.guess && chmod +x config.guess
<<<<<<< HEAD
./configure --enable-fat \
            --enable-shared \
=======

./configure ${CONFIG_ARGS}

make --silent all install

cd ..

ZZ_VERSION=0.9.0a4
ZZ_DIR=zz-${ZZ_VERSION}
ZZ_URL=https://github.com/diofant/zz/releases/download/v${ZZ_VERSION}/${ZZ_DIR}.tar.gz

download ${ZZ_URL}
tar --extract --file ${ZZ_DIR}.tar.gz
cd ${ZZ_DIR}

if [ "$OSTYPE" = "cygwin" ] && [ "${RUNNER_ARCH}" = "ARM64" ]
then
  autoreconf -if
fi

./configure --enable-shared \
>>>>>>> 33cfa0e (Use libzz v0.9.0a4)
            --disable-static \
            --with-pic \
            --disable-alloca \
            --prefix=$PREFIX -q
make -j6 -s
make -s install
cd ../

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
./configure --enable-fat \
            --enable-shared \
            --disable-static \
            --with-pic \
            --disable-alloca \
            --prefix=$PREFIX -q
make -j6 -s
make -s install
cd ../

#!/bin/bash

set -e -x

GMP_VERSION=6.3.0

PREFIX="$(pwd)/.local/"

# -- build GMP --
curl -s -O https://ftp.gnu.org/gnu/gmp/gmp-${GMP_VERSION}.tar.xz
tar -xf gmp-${GMP_VERSION}.tar.xz
cd gmp-${GMP_VERSION}
patch -N -Z -p0 < ../scripts/fat_build_fix.diff
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

# -- generate *.lib files from *.dll on M$ Windows --
if [ "$OSTYPE" = "msys" ]
then
  # Set path to dumpbin & lib
  PATH="$PATH:$(find "/c/Program Files/Microsoft Visual Studio/2022/" -name "Hostx86")/x64/"

  # See http://stackoverflow.com/questions/9946322/
  cd .local/bin
  dll_file=libgmp-10.dll
  lib_name=$(basename -s .dll ${dll_file})
  exports_file=${lib_name}-exports.txt
  def_file=${lib_name}.def
  lib_file=${lib_name}.lib
  name=$(echo ${lib_name}|sed 's/^lib//;s/-[0-9]\+//')

  dumpbin //exports ${dll_file} > ${exports_file}

  echo LIBRARY ${lib_name} > ${def_file}
  echo EXPORTS >> ${def_file}
  cat ${exports_file} | awk 'NR>19 && $4 != "" {print $4 " @"$1}' >> ${def_file}
  sed -i 's/$/\r/' ${def_file}

  lib //def:${def_file} //out:${lib_file} //machine:x64

  rm ${exports_file} ${def_file} ${lib_name}.exp
  mv ${lib_file} ${name}.lib

  cd ..
  cp -av bin lib
fi

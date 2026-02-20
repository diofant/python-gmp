#!/bin/sh

set -e -x

PREFIX=${PREFIX:-"$(pwd)/.local"}
CFLAGS=
CURL_OPTS="--fail --location --retry 4 --connect-timeout 32"

download () {
  sleep_ivl=16
  until curl ${CURL_OPTS} --remote-name $1
  do
    sleep ${sleep_ivl}
    sleep_ivl=$((${sleep_ivl}*2))
  done
}

GMP_VERSION=6.3.0
GMP_DIR=gmp-${GMP_VERSION}
GMP_URL=https://ftp.gnu.org/gnu/gmp/${GMP_DIR}.tar.xz

download ${GMP_URL}
tar --extract --file ${GMP_DIR}.tar.xz
cd ${GMP_DIR}

for f in ../scripts/*.diff
do
  patch --strip 1 < $f
done

CONFIG_ARGS="--enable-shared --disable-static --with-pic --disable-alloca --prefix=$PREFIX"
if [ "$OSTYPE" = "cygwin" ]
then
  if [ "${RUNNER_ARCH}" = "ARM64" ]
  then
    autoreconf -fi
    CONFIG_ARGS="${CONFIG_ARGS} --disable-assembly"
  else
    CONFIG_ARGS="${CONFIG_ARGS} --enable-fat"
  fi
else
  CONFIG_ARGS="${CONFIG_ARGS} --enable-fat"
fi

# config.guess uses microarchitecture and configfsf.guess doesn't
# We replace config.guess with configfsf.guess to avoid microarchitecture
# specific code in common code.
rm config.guess && mv configfsf.guess config.guess && chmod +x config.guess

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
            --disable-static \
            --with-pic \
            --with-gmp=$PREFIX \
            --prefix=$PREFIX

make --silent all install

cd ../

# -- generate *.lib files from *.dll on M$ Windows --
if [ "$OSTYPE" = "cygwin" ]
then
  cd .local/bin
  for dll_file in libgmp-10.dll libzz-0.dll
  do
    lib_name=$(basename -s .dll ${dll_file})
    name=$(echo ${lib_name}|sed 's/^lib//;s/-[0-9]\+//')

    gendef ${dll_file}
    dlltool -d ${lib_name}.def -l ${name}.lib

    cp ${name}.lib ../lib/
    cp ${dll_file} ../lib/
  done
fi

cd ../..

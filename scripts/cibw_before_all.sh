#!/bin/sh

set -e -x

GMP_VERSION=6.3.0
GMP_DIR=gmp-${GMP_VERSION}
GMP_URL=https://ftp.gnu.org/gnu/gmp/${GMP_DIR}.tar.xz
PREFIX="$(pwd)/.local/"
CFLAGS=

curl --retry 3 --silent --remote-name ${GMP_URL}
tar --extract --file ${GMP_DIR}.tar.xz
cd ${GMP_DIR}

for f in ../scripts/*.diff
do
  patch --strip 1 < $f
done

CONFIG_ARGS="--enable-shared --disable-static --with-pic --prefix=$PREFIX"
if [ "$OSTYPE" = "msys" ] || [ "$OSTYPE" = "cygwin" ]
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

ZZ_VERSION=0.6.1
ZZ_DIR=zz-${ZZ_VERSION}
ZZ_URL=https://github.com/diofant/zz/releases/download/v${ZZ_VERSION}/${ZZ_DIR}.tar.gz

curl --location --retry 3 --silent --remote-name ${ZZ_URL}
tar --extract --file ${ZZ_DIR}.tar.gz
cd ${ZZ_DIR}

./configure --quiet \
            --enable-shared \
            --disable-static \
            --with-pic \
            --with-gmp=$PREFIX \
            --prefix=$PREFIX

make --silent all install

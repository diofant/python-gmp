#!/bin/sh

set -o errexit

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
genlib () {
  # -- generate *.lib files from *.dll on M$ Windows --
  cd .local/bin/
  dll_file=$1
  lib_name=$(basename -s .dll ${dll_file})
  name=$(echo ${lib_name}|sed 's/^lib//;s/-[0-9]\+//')

  gendef ${dll_file}
  dlltool -d ${lib_name}.def -l ${name}.lib

  cp ${name}.lib ../lib/
  cp ${dll_file} ../lib/
  cd ../../
}

SKIP_GMP=no

while [ $# -gt 0 ]
do
  key="$1"
  case $key in
    -h|--help)
      echo "scripts/cibw_before_all.sh [options]"
      echo
      echo "Build local installs of python-gmp's dependencies."
      echo
      echo "Supported options:"
      echo "  --help            - show this help message"
      echo "  --skip-gmp        - skip building GMP"
      echo
      exit
    ;;
    --skip-gmp)
      # If you already have a local install of GMP you can pass --skip-gmp
      # to skip building it.
      SKIP_GMP=yes
      shift
    ;;
  *)
    2>&1 echo "unrecognised argument:" $key
    exit 1
    ;;
  esac
done

set -o xtrace

if [ $SKIP_GMP = "no" ]
then
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

  if [ "$OSTYPE" = "cygwin" ]
  then
    genlib libgmp-10.dll
  fi
fi

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

CONFIG_ARGS="--enable-shared --disable-static --with-pic --prefix=$PREFIX"
if [ $SKIP_GMP = "no" ]
then
  CONFIG_ARGS="${CONFIG_ARGS} --with-gmp=$PREFIX"
fi

./configure ${CONFIG_ARGS}

make --silent all install

cd ../

if [ "$OSTYPE" = "cygwin" ]
then
  genlib libzz-0.dll
fi

#!/bin/bash

DEST_DIR=$1
WHEEL=$2
LD_LIBRARY_PATH="$(pwd)/.local/lib:$LD_LIBRARY_PATH"

unzip -l ${WHEEL}

if [[ "$OSTYPE" == "darwin"* ]]
then
  delocate-wheel --lib-sdir ../python-gmp.libs -w ${DEST_DIR} -v ${WHEEL}
else
  auditwheel repair -w ${DEST_DIR} ${WHEEL}
fi

unzip -l ${WHEEL}

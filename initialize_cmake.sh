#!/bin/bash

OPT="-DCMAKE_BUILD_TYPE=RelWithDebInfo"
MSG="${CI_COMMIT_MESSAGE}"

if [[ $MSG == *"[gcc]"* ]]; then
  GCC="ON"
else
  GCC="OFF"
fi

if [[ $MSG == *"[clang]"* ]]; then
  CLANG="ON"
else
  CLANG="OFF"
fi

if [[ $MSG == *"[func]"* ]]; then
  FUNC="ON"
else
  FUNC="OFF"
fi

cmake ${OPT} -DRUN_GCC=${GCC} -DRUN_CLANG=${CLANG} -DFUNC_TEST=${FUNC} ..

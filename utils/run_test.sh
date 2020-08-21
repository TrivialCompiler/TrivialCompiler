#!/bin/bash

mkdir -p build && cd build
bash ../utils/initialize_cmake.sh
make -j4
bash ../utils/build_oneliner.sh
./TrivialCompiler -p
env CTEST_OUTPUT_ON_FAILURE=1 ctest

#!/bin/bash

MSG="${CI_COMMIT_MESSAGE}"

if [[ $MSG == *"[oneline]"* ]]; then
  cd ..
  BUILD=$(java -cp utils CommandGenerator src/ build/TrivialCompiler)
  echo $BUILD
  eval $BUILD
  cd build
fi

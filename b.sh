#!/bin/bash
# now works woth llvm 17
# TODO to make it works woth llvm 20
rm -rf _release/ && CXX=clang++ cmake_release  && /bin/cmake --build _release/ -j12

#!/bin/sh
# Build universal (arm64 + x86_64) binaries into dist/ and ad-hoc sign them.
set -eu
cd "$(dirname "$0")"
mkdir -p dist
clang -arch x86_64 -arch arm64 -O2 -Wall -dynamiclib -o dist/precise_sleep.dylib src/precise_sleep.c
clang -arch x86_64 -arch arm64 -O2 -Wall -o dist/precise_run src/precise_run.c
codesign -s - -f dist/precise_sleep.dylib dist/precise_run
lipo -info dist/precise_sleep.dylib dist/precise_run

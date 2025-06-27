#!/usr/bin/env bash
export LC_ALL=C.UTF-8
set -o errexit -o nounset -o pipefail -o xtrace

[ "${CI_CONFIG+x}" ] && source "$CI_CONFIG"

: "${CI_DIR:=build}"
if ! [ -v BUILD_TARGETS ]; then
  BUILD_TARGETS=(all tests mpexamples)
fi

[ -n "${CI_CLEAN-}" ] && rm -rf "${CI_DIR}"

cmake -B "$CI_DIR" "${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"}"
cmake --build "$CI_DIR" -t "${BUILD_TARGETS[@]}" -- "${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}"
ctest --test-dir "$CI_DIR" --output-on-failure

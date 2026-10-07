#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
seconds=${1:-60}
if [[ ! "$seconds" =~ ^[1-9][0-9]*$ ]]; then
  echo 'Expected a positive number of seconds per target' >&2
  exit 2
fi
out=.pio/fuzz
mkdir -p "$out"
includes=()
for directory in lib/*/src; do includes+=("-I$directory"); done
sources=()
for source in lib/*/src/*.cpp; do
  [[ "$source" == */cajui_nvs.cpp ]] || sources+=("$source")
done
for target in frames commands; do
  "${CXX:-clang++}" -std=c++11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
    "${includes[@]}" "${sources[@]}" "test/fuzz/fuzz_$target.cpp" -lcrypto -o "$out/fuzz_$target"
  mkdir -p "$out/corpus_$target" "$out/artifacts/$target"
  cp test/fuzz/seeds/"$target"/* "$out/corpus_$target/"
  "$out/fuzz_$target" -max_total_time="$seconds" -max_len=600 \
    -artifact_prefix="$out/artifacts/$target/" "$out/corpus_$target"
done

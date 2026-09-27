#!/bin/sh
# Coverage-guided fuzzing of the untrusted-input parsers with libFuzzer. Needs a clang with
# libFuzzer (Apple's clang has none; CI runs it on Linux) and OpenSSL. Usage:
#   sh scripts/fuzz.sh [seconds per target]
set -eu
cd "$(dirname "$0")/.."
seconds=${1:-60}
out=.pio/fuzz
mkdir -p "$out"
includes=$(for d in lib/*/src; do printf ' -I%s' "$d"; done)
sources=$(ls lib/*/src/*.cpp | grep -v cajui_nvs.cpp)
for target in frames commands; do
  # shellcheck disable=SC2086 # The lists are word-split on purpose.
  clang++ -std=c++11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
    $includes $sources "test/fuzz/fuzz_$target.cpp" -lcrypto -o "$out/fuzz_$target"
  mkdir -p "$out/corpus_$target"
  "$out/fuzz_$target" -max_total_time="$seconds" -max_len=600 "$out/corpus_$target"
done

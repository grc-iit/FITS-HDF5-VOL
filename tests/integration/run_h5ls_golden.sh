#!/usr/bin/env bash
# M2.8 golden test: run h5ls -r on a fixture and diff against the recorded
# golden output. CMake invokes this with three args.
set -euo pipefail
H5LS="$1"
FIXTURE="$2"
GOLDEN="$3"

actual=$("$H5LS" -r "$FIXTURE")
expected=$(cat "$GOLDEN")

if [ "$actual" != "$expected" ]; then
    echo "FAIL: h5ls -r output for $FIXTURE differs from golden $GOLDEN"
    diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual")
    exit 1
fi
echo "OK: $FIXTURE matches $GOLDEN"

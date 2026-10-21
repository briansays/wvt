#!/bin/sh
# Runs clang-tidy on wvt's own sources, as the CI job does (issue #54).
#   cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#   pip install clang-tidy==23.1.3
#   tools/run-clang-tidy.sh build
# The checks are in .clang-tidy; any finding is an error (exit code not 0).
set -e
build=${1:-build}
tidy=${CLANG_TIDY:-clang-tidy}
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
python3 - "$build/compile_commands.json" <<'PY' | xargs -n 1 -P "$jobs" "$tidy" -p "$build" --quiet
import json, os, sys
files = sorted({e["file"] for e in json.load(open(sys.argv[1]))})
skip = ("/third_party/",)
# Files generated in the build folder (the font, build_info.cpp) are not ours
# to check, and a configure-only build does not have them yet.
gen = os.path.dirname(os.path.realpath(sys.argv[1])) + os.sep
print("\n".join(f for f in files if not any(s in f for s in skip) and not os.path.realpath(f).startswith(gen)
                 and not f.endswith((".c", ".cu"))))
PY

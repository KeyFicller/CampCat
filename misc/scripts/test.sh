#!/bin/sh
# The gate, runnable locally. Same three steps CI runs; any failure is a non-zero
# exit. Needs a configured clang-p2996 and .venv (see CMakeLists.txt, ReadMe).
set -e
cd "$(dirname "$0")/../.."

# Configure through the machine-local preset when it exists: it carries
# CPPREF_LLVM_ROOT and the git mirror rewrites (see CMakeUserPresets.json.example).
# Without it, configure plainly and let CPPREF_LLVM_ROOT come from the
# environment or a -D. Both land in ./build, so the steps below are the same.
if [ -f CMakeUserPresets.json ]; then
    cmake --preset local
else
    cmake -S . -B build
fi
cmake --build build
# After the build: `gtest_discover_tests` learns the test list by running the
# binary, so ctest before that finds nothing.
ctest --test-dir build --output-on-failure
# `uv`, not `pip`: .venv is uv's, and uv-managed environments have no pip in them.
uv pip install --python .venv/bin/python3 -r requirements.txt
.venv/bin/python3 -m pytest campcat/tests/py -q

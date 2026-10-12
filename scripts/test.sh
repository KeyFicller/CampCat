#!/bin/sh
# The gate, runnable locally. Same three steps CI runs; any failure is a non-zero
# exit. Needs a configured clang-p2996 and .venv (see CMakeLists.txt, ReadMe).
set -e
cd "$(dirname "$0")/.."

cmake -S . -B build
cmake --build build
# After the build: `gtest_discover_tests` learns the test list by running the
# binary, so ctest before that finds nothing.
ctest --test-dir build --output-on-failure

# `uv`, not `pip`: .venv is uv's, and uv-managed environments have no pip in them.
uv pip install --python .venv/bin/python3 -r requirements.txt
.venv/bin/python3 -m pytest tests/py -q

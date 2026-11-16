#!/bin/sh
# Cache the same Bloomberg revision as the Linux toolchain image.
set -eu

revision=9ffb96e3ce36
: "${CPPREF_LLVM_ROOT:?Set CPPREF_LLVM_ROOT to the compiler install prefix}"
: "${CAMPCAT_TOOLCHAIN_WORK:?Set CAMPCAT_TOOLCHAIN_WORK to a temporary build directory}"
test "$(uname -s)" = Darwin
mkdir -p "$CAMPCAT_TOOLCHAIN_WORK"

if [ ! -f "$CPPREF_LLVM_ROOT/.campcat-revision" ] ||
   [ "$(cat "$CPPREF_LLVM_ROOT/.campcat-revision")" != "$revision" ]; then
    case "$(uname -m)" in
        arm64) target=AArch64 ;;
        x86_64) target=X86 ;;
        *) echo "Unsupported macOS architecture" >&2; exit 1 ;;
    esac
    curl --fail --location --retry 3 \
        "https://github.com/bloomberg/clang-p2996/archive/$revision.tar.gz" \
        -o "$CAMPCAT_TOOLCHAIN_WORK/source.tar.gz"
    mkdir -p "$CAMPCAT_TOOLCHAIN_WORK/source"
    tar -xzf "$CAMPCAT_TOOLCHAIN_WORK/source.tar.gz" \
        -C "$CAMPCAT_TOOLCHAIN_WORK/source" --strip-components=1
    cmake -S "$CAMPCAT_TOOLCHAIN_WORK/source/llvm" \
        -B "$CAMPCAT_TOOLCHAIN_WORK/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$CPPREF_LLVM_ROOT" \
        -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)" \
        -DLLVM_ENABLE_PROJECTS=clang \
        '-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind' \
        -DLLVM_TARGETS_TO_BUILD="$target" \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
        -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_ENABLE_ASSERTIONS=OFF
    cmake --build "$CAMPCAT_TOOLCHAIN_WORK/build" --parallel 3
    cmake --install "$CAMPCAT_TOOLCHAIN_WORK/build"
fi

# Run on cache hits too: a compiler without its matching headers/runtime fails.
test -f "$CPPREF_LLVM_ROOT/include/c++/v1/meta"
test -f "$CPPREF_LLVM_ROOT/lib/libc++.dylib"
cat > "$CAMPCAT_TOOLCHAIN_WORK/probe.cpp" <<'CPP'
#include <meta>
static_assert(std::meta::is_type(^^int));
int main() {}
CPP
"$CPPREF_LLVM_ROOT/bin/clang++" -std=c++26 -freflection-latest \
    -isysroot "$(xcrun --show-sdk-path)" \
    -L "$CPPREF_LLVM_ROOT/lib" "-Wl,-rpath,$CPPREF_LLVM_ROOT/lib" \
    "$CAMPCAT_TOOLCHAIN_WORK/probe.cpp" -o "$CAMPCAT_TOOLCHAIN_WORK/probe"
"$CAMPCAT_TOOLCHAIN_WORK/probe"
printf '%s\n' "$revision" > "$CPPREF_LLVM_ROOT/.campcat-revision"

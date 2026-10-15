#!/bin/bash
# Prime every FetchContent dependency into a shared cache, sequentially.
# Sequential on purpose: through the mirror, one stream runs at ~3 MB/s while
# eight concurrent ones stalled to nothing.
#
# Lands in <cache>/<name>-src, which is exactly where FetchContent looks when
# FETCHCONTENT_BASE_DIR points at <cache>, so a later clean build reuses it.
# Each clone goes to a temp dir and is moved into place, so a failure can never
# delete a cache entry that is already good.
set -u
# Straight to github.com. Override MIRROR only if a redirect is needed; note
# that a prefix-style proxy (what the old default was) is not the same thing as
# git's url.insteadOf, which the CMakeUserPresets.json "local" preset uses.
M="${MIRROR:-https://github.com}"
CACHE="${1:?cache dir}"
mkdir -p "$CACHE"
TMP="$CACHE/.tmp.$$"
mkdir -p "$TMP"

have() { [ -d "$CACHE/$1-src/.git" ]; }

finish() { # name tmpdir
    if have "$1"; then rm -rf "$2"; echo "[skip] $1 (already in cache)"; return 0; fi
    rm -rf "$CACHE/$1-src"
    mv "$2" "$CACHE/$1-src"
}

tag_clone() { # name repo tag
    local name="$1" repo="$2" tag="$3" t0 t1 d="$TMP/$1"
    if have "$name"; then echo "[skip] $name (already in cache)"; return; fi
    t0=$(date +%s)
    if git clone -q --depth 1 --branch "$tag" --single-branch \
           "$M/$repo" "$d" 2>/dev/null; then
        t1=$(date +%s)
        finish "$name" "$d"
        echo "[ ok ] $name tag=$tag  $((t1-t0))s  $(du -sh "$CACHE/$name-src" | cut -f1)"
    else
        rm -rf "$d"
        echo "[FAIL] $name tag=$tag"
    fi
}

sha_clone() { # name repo sha
    local name="$1" repo="$2" sha="$3" t0 t1
    local d="$TMP/$1"
    if have "$name"; then echo "[skip] $name (already in cache)"; return; fi
    t0=$(date +%s)
    if git init -q "$d" &&
       git -C "$d" remote add origin "$M/$repo" &&
       git -C "$d" fetch -q --depth 1 origin "$sha" &&
       git -C "$d" checkout -q FETCH_HEAD; then
        t1=$(date +%s)
        finish "$name" "$d"
        echo "[ ok ] $name sha=${sha:0:8}  $((t1-t0))s  $(du -sh "$CACHE/$name-src" | cut -f1)"
    else
        rm -rf "$d"
        echo "[FAIL] $name sha=${sha:0:8}"
    fi
}

# Same repos, tags and commits as CMakeLists.txt.
tag_clone glfw       glfw/glfw.git         3.4
tag_clone imgui      ocornut/imgui.git     v1.91.8
tag_clone json       nlohmann/json.git     v3.11.3
tag_clone googletest google/googletest.git v1.15.2
sha_clone cppreflect26 KeyFicller/CppReflectKit.git        1ac4c8b21624b401862cdde703e8478e86f9159d
tag_clone cpp_httplib  yhirose/cpp-httplib.git v0.60.1
tag_clone cppcodec     tplgy/cppcodec.git      v0.2
sha_clone portable_file_dialogs samhocevar/portable-file-dialogs.git c12ea8c9a727f5320a2b4570aee863bbede2a204
sha_clone imgui_markdown juliettef/imgui_markdown.git      4acbf80584753e15ea54eb271129995862daac8f
sha_clone imgui_colortextedit KeyFicller/ImGuiColorTextEdit.git 14fdfd4c2f19846aac6c6f8010899021022a9c16
sha_clone imguizmo CedricGuillemet/ImGuizmo.git b796ac3b861afc6e91ca74e4611effd9c9527367

rmdir "$TMP" 2>/dev/null
echo "--- cache total ---"
du -sh "$CACHE"

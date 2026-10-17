cmake_minimum_required(VERSION 3.20)

# clang-p2996 (P2996 static reflection), installed locally:
#   <CPPREF_LLVM_ROOT>/bin/clang++          compiler
#   <CPPREF_LLVM_ROOT>/include/c++/v1/meta  reflective libc++
# The tool schemas handed to the model are generated from C++ declarations, so
# the whole project is pinned to this compiler. See
# docs/superpowers/specs/2026-10-09-llm-schema-bridge-design.md section 6.
#
# The prefix is machine-local, so it is not written into this file: the
# gitignored CMakeUserPresets.json "local" preset exports CPPREF_LLVM_ROOT (the
# same variable CppReflectKit reads), and -DCPPREF_LLVM_ROOT=/path wins over it.
if(NOT CPPREF_LLVM_ROOT AND DEFINED ENV{CPPREF_LLVM_ROOT})
  set(CPPREF_LLVM_ROOT "$ENV{CPPREF_LLVM_ROOT}")
endif()
if(NOT EXISTS "${CPPREF_LLVM_ROOT}/bin/clang++" OR
   NOT EXISTS "${CPPREF_LLVM_ROOT}/include/c++/v1/meta")
  message(FATAL_ERROR
    "clang-p2996 not found at \"${CPPREF_LLVM_ROOT}\" (need bin/clang++ and "
    "include/c++/v1/meta); set CPPREF_LLVM_ROOT, as the CMakeUserPresets.json "
    "\"local\" preset does, or pass -DCPPREF_LLVM_ROOT=/path")
endif()
set(CMAKE_CXX_COMPILER "${CPPREF_LLVM_ROOT}/bin/clang++" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER "${CPPREF_LLVM_ROOT}/bin/clang" CACHE FILEPATH "" FORCE)

project(campcat LANGUAGES CXX)

# This file stays at the repo root with the other dependency declarations
# (`requirements.txt`, `.python-version`); everything else lives under
# `campcat/sources/`, split by language, and `config.json` is root-level too.
# So `CMAKE_CURRENT_SOURCE_DIR` is the repo root throughout.

if(APPLE)
  # A source-built clang has no default sysroot, so it cannot even find the
  # macOS SDK headers without this. Linux has no xcrun and an empty -isysroot
  # is only noise there.
  execute_process(COMMAND xcrun --show-sdk-path
                  OUTPUT_VARIABLE _campcat_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
  add_compile_options(-isysroot "${_campcat_sdk}")
endif()
# Reflection is a C++ language feature; Objective-C++ does not need it.
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-freflection-latest>")
# Link the reflective runtime from the prefix instead of /usr/lib.
add_link_options(-L "${CPPREF_LLVM_ROOT}/lib" "-Wl,-rpath,${CPPREF_LLVM_ROOT}/lib")

# The macOS IME candidate-window fix needs Objective-C++.
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# OpenCV: install via Homebrew (`brew install opencv`), then configure with CMake.
# Apple Silicon hint if needed: -DOpenCV_DIR=/opt/homebrew/lib/cmake/opencv4
# freetype draws the CJK label `mark` paints onto a screenshot; cv::putText
# itself only knows ASCII.
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs freetype)

# misc/scripts/prime_deps.sh pre-clones every dependency into this cache. When
# it is present, configure from it offline instead of re-cloning (minutes ->
# seconds); the script must be re-run after a new dependency is declared. Absent
# (fresh machine, CI) leaves the normal download path untouched.
#
# This must come before include(FetchContent): that module sets both variables
# itself, and `set(... CACHE ...)` / option() never override a value already set.
if(NOT FETCHCONTENT_BASE_DIR AND EXISTS "$ENV{HOME}/.cache/campcat-deps/glfw-src")
  set(FETCHCONTENT_BASE_DIR "$ENV{HOME}/.cache/campcat-deps" CACHE PATH
    "Primed dependency cache filled by misc/scripts/prime_deps.sh")
  set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL
    "Reuse the primed cache and keep FetchContent off the network")
endif()

include(FetchContent)

# Dependencies come straight from github.com. A network that cannot reach it
# redirects through git's own rewrite rules instead: the gitignored
# CMakeUserPresets.json "local" preset exports GIT_CONFIG_* url.insteadOf
# entries, the same convention CppReflectKit uses. Keep such a redirect out of
# this file so a checked-in build stays host-agnostic.

# Shallow clones: full histories made imgui/json .git dirs ~250MB each and
# re-cloning missing glfw-src dominated configure time.
FetchContent_Declare(
  glfw
  GIT_REPOSITORY "https://github.com/glfw/glfw.git"
  GIT_TAG        3.4
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY "https://github.com/ocornut/imgui.git"
  GIT_TAG        v1.91.8
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
  json
  GIT_REPOSITORY "https://github.com/nlohmann/json.git"
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(json)

# The shared reflection layer (schema generation + strict argument binding),
# consumed instead of kept in tree. Header-only INTERFACE targets; UI is off
# because CampCat builds its own imgui_lib, and the library's tests are its own
# business. nlohmann is already provided above, so the library keeps quiet.
set(CPPREF_WITH_UI     OFF CACHE BOOL "" FORCE)
set(CPPREF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Its mcp target pulls cpp-httplib and cppcodec, which CampCat does not link. It
# keeps CPPREF_WITH_LLM on all the same, because cppreflect::json -- the target
# CampCat does link -- hangs off that option. Upstream fetches both deps itself
# from github.com; it used to expose CPPREF_HTTPLIB_URL / CPPREF_CPPCODEC_URL to
# redirect them, and dropped that when it dropped its mirrors.
FetchContent_Declare(
  cppreflect26
  GIT_REPOSITORY "https://github.com/KeyFicller/CppReflectKit.git"
  GIT_TAG        1ac4c8b21624b401862cdde703e8478e86f9159d
  GIT_PROGRESS   TRUE
)
# No GIT_SHALLOW: a pinned commit is not on a branch tip, and a shallow fetch of
# one is not something every server allows.
FetchContent_MakeAvailable(cppreflect26)

# Header-only native file dialogs (Win / macOS / Linux).
# Their CMakeLists still declares an old cmake_minimum_required; force policy floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
  portable_file_dialogs
  GIT_REPOSITORY "https://github.com/samhocevar/portable-file-dialogs.git"
  GIT_TAG        c12ea8c9a727f5320a2b4570aee863bbede2a204
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(portable_file_dialogs)

# Markdown rendering for the LLM reply bubbles. Upstream ships only a header,
# so there is no CMakeLists.txt for FetchContent to add; the include directory
# is exposed through an INTERFACE target instead.
FetchContent_Declare(
  imgui_markdown
  GIT_REPOSITORY "https://github.com/juliettef/imgui_markdown.git"
  GIT_TAG        4acbf80584753e15ea54eb271129995862daac8f
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(imgui_markdown)

add_library(imgui_markdown INTERFACE)
target_include_directories(imgui_markdown INTERFACE ${imgui_markdown_SOURCE_DIR})

# The script editor's text widget. Same shape as imgui_markdown above: upstream
# ships no CMakeLists.txt, so FetchContent only populates and the source is
# compiled into a small static library of our own.
FetchContent_Declare(
  imgui_colortextedit
  GIT_REPOSITORY "https://github.com/KeyFicller/ImGuiColorTextEdit.git"
  GIT_TAG        14fdfd4c2f19846aac6c6f8010899021022a9c16
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(imgui_colortextedit)

# ImGuizmo's GraphEditor: the canvas the Graph page draws. Only that widget is
# wanted, so `src` is named as the subdirectory to add -- it has no
# CMakeLists.txt, so FetchContent only populates and the rest of the repo's
# widgets stay out of the build.
FetchContent_Declare(
  imguizmo
  GIT_REPOSITORY "https://github.com/CedricGuillemet/ImGuizmo.git"
  GIT_TAG        b796ac3b861afc6e91ca74e4611effd9c9527367
  GIT_PROGRESS   TRUE
  SOURCE_SUBDIR  src
)
# No GIT_SHALLOW: a pinned commit is not on a branch tip, and a shallow fetch of
# one is not something every server allows.
FetchContent_MakeAvailable(imguizmo)

find_package(OpenGL REQUIRED)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

add_library(imgui_colortextedit_lib STATIC
  ${imgui_colortextedit_SOURCE_DIR}/TextEditor.cpp
  campcat/sources/cpp/src/app/ui/ccat_highlight.cpp
)
target_include_directories(imgui_colortextedit_lib PUBLIC
  ${imgui_colortextedit_SOURCE_DIR}
)
target_include_directories(imgui_colortextedit_lib PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/campcat/sources/cpp/include
)
target_link_libraries(imgui_colortextedit_lib PUBLIC imgui_lib)

# Same shape as the widget above: upstream's CMakeLists compiles every widget it
# ships, and only GraphEditor is wanted.
add_library(imgui_grapheditor_lib STATIC
  ${imguizmo_SOURCE_DIR}/src/GraphEditor.cpp
)
target_include_directories(imgui_grapheditor_lib PUBLIC
  ${imguizmo_SOURCE_DIR}/src
)
target_link_libraries(imgui_grapheditor_lib PUBLIC imgui_lib)

# Core framework + Dear ImGui shell + the ccat driver
add_library(campcat_core STATIC
  campcat/sources/cpp/src/core/adb/adb_client.cpp
  campcat/sources/cpp/src/core/mumu/mumu_launcher.cpp
  campcat/sources/cpp/src/core/vision/template_matcher.cpp
  campcat/sources/cpp/src/core/vision/screen_template_ops.cpp
  campcat/sources/cpp/src/core/vision/box_snap.cpp
  campcat/sources/cpp/src/core/vision/region_record.cpp
  campcat/sources/cpp/src/core/config/app_config.cpp
  campcat/sources/cpp/src/core/automation/automation_factory.cpp
  campcat/sources/cpp/src/core/app/scheduler.cpp
  campcat/sources/cpp/src/core/app/log_buffer.cpp
  campcat/sources/cpp/src/core/app/automation_log.cpp
  campcat/sources/cpp/src/core/script/ccat_lexer.cpp
  campcat/sources/cpp/src/core/script/ccat_bundle.cpp
  campcat/sources/cpp/src/core/script/ccat_graph.cpp
  campcat/sources/cpp/src/core/script/ccat_parser.cpp
  campcat/sources/cpp/src/core/script/ccat_interpreter.cpp
  campcat/sources/cpp/src/core/script/ccat_repl.cpp
  campcat/sources/cpp/src/core/process.cpp
  campcat/sources/cpp/src/core/llm/llm_protocol.cpp
  campcat/sources/cpp/src/core/llm/llm_memory.cpp
  campcat/sources/cpp/src/core/llm/llm_tools.cpp
  campcat/sources/cpp/src/core/llm/llm_host.cpp
  campcat/sources/cpp/src/ccat_driver/ccat_script_profile.cpp
  campcat/sources/cpp/src/ccat_driver/ccat_script_automation.cpp
  campcat/sources/cpp/src/ccat_driver/ccat_program_runner.cpp
)
target_include_directories(campcat_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/campcat/sources/cpp/include
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(campcat_core PUBLIC
  ${OpenCV_LIBS}
  nlohmann_json::nlohmann_json
  cppreflect::json
)

# The IME candidate-window fix is Objective-C++, so it stays Apple-only.
set(campcat_platform_sources)
if(APPLE)
  list(APPEND campcat_platform_sources campcat/sources/cpp/src/app/ui/macos_ime.mm)
endif()

add_executable(campcat
  campcat/sources/cpp/src/app/main.cpp
  campcat/sources/cpp/src/app/shell_state.cpp
  campcat/sources/cpp/src/app/ui/llm_panel.cpp
  campcat/sources/cpp/src/app/ui/log_panel.cpp
  campcat/sources/cpp/src/app/ui/console_panel.cpp
  campcat/sources/cpp/src/app/ui/script_panel.cpp
  campcat/sources/cpp/src/app/ui/settings_panel.cpp
  campcat/sources/cpp/src/app/ui/script_editor_panel.cpp
  campcat/sources/cpp/src/app/ui/template_capture_panel.cpp
  campcat/sources/cpp/src/app/ui/graph_panel.cpp
  campcat/sources/cpp/src/app/ui/prompt_input.cpp
  campcat/sources/cpp/src/app/ui/fonts.cpp
  campcat/sources/cpp/src/app/ui/gl_texture.cpp
  campcat/sources/cpp/src/app/ui/icon_texture.cpp
  ${campcat_platform_sources}
)
target_include_directories(campcat PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/campcat/sources/cpp/include
  ${portable_file_dialogs_SOURCE_DIR}
)
target_link_libraries(campcat PRIVATE campcat_core imgui_lib glfw OpenGL::GL
                                      imgui_markdown imgui_colortextedit_lib
                                      imgui_grapheditor_lib)

if(APPLE)
  target_link_libraries(campcat PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
endif()

target_compile_definitions(campcat PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

# Test gate. `include(CTest)` is what defines BUILD_TESTING and gives `ctest`
# something to read, so config a build with -DBUILD_TESTING=OFF to get exactly
# what shipped before this block existed.
include(CTest)
if(BUILD_TESTING)
  # Same source as every other dependency: github.com, redirected by the
  # developer's git rewrite rules when the network needs it.
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY "https://github.com/google/googletest.git"
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_SUBMODULES ""
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  # The second argument matters: without it the binary dir would mirror the
  # source path and create `build/campcat/`, which collides with the `campcat`
  # executable the linker writes at `build/campcat`.
  add_subdirectory(campcat/tests tests)
endif()

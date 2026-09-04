cmake_minimum_required(VERSION 3.20)
project(campcat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# OpenCV: install via Homebrew (`brew install opencv`), then configure with CMake.
# Apple Silicon hint if needed: -DOpenCV_DIR=/opt/homebrew/lib/cmake/opencv4
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

include(FetchContent)

# GitHub often unreachable; prefix FetchContent clones via a mirror.
# Direct:  cmake -DGITHUB_MIRROR=
# Alts:    https://ghproxy.net/https://github.com
#          https://hub.gitmirror.com/https://github.com
set(GITHUB_MIRROR "https://ghfast.top/https://github.com" CACHE STRING
  "Prefix for GitHub FetchContent URLs (empty = https://github.com)")

function(campcat_github_url out_var repo_path)
  if(GITHUB_MIRROR STREQUAL "")
    set(${out_var} "https://github.com/${repo_path}" PARENT_SCOPE)
  else()
    set(${out_var} "${GITHUB_MIRROR}/${repo_path}" PARENT_SCOPE)
  endif()
endfunction()

# Shallow clones: full histories made imgui/json .git dirs ~250MB each and
# re-cloning missing glfw-src dominated configure time.
campcat_github_url(_glfw_git glfw/glfw.git)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY ${_glfw_git}
  GIT_TAG        3.4
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

campcat_github_url(_imgui_git ocornut/imgui.git)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY ${_imgui_git}
  GIT_TAG        v1.91.8
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(imgui)

campcat_github_url(_json_git nlohmann/json.git)
FetchContent_Declare(
  json
  GIT_REPOSITORY ${_json_git}
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(json)

# Header-only native file dialogs (Win / macOS / Linux).
# Their CMakeLists still declares an old cmake_minimum_required; force policy floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
campcat_github_url(_pfd_git samhocevar/portable-file-dialogs.git)
FetchContent_Declare(
  portable_file_dialogs
  GIT_REPOSITORY ${_pfd_git}
  GIT_TAG        c12ea8c9a727f5320a2b4570aee863bbede2a204
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(portable_file_dialogs)

find_package(OpenGL REQUIRED)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

# Core framework + Dear ImGui shell + ccat_script driver
add_library(campcat_core STATIC
  src/core/adb/adb_client.cpp
  src/core/vision/template_matcher.cpp
  src/core/vision/screen_template_ops.cpp
  src/core/config/app_config.cpp
  src/core/automation/automation_factory.cpp
  src/core/app/scheduler.cpp
  src/core/app/log_buffer.cpp
  src/core/app/automation_log.cpp
  src/core/script/ccat_lexer.cpp
  src/core/script/ccat_parser.cpp
  src/core/script/ccat_interpreter.cpp
  src/core/script/ccat_repl.cpp
  src/ccat_script/ccat_script_profile.cpp
  src/ccat_script/ccat_script_automation.cpp
  src/ccat_script/ccat_program_runner.cpp
)
target_include_directories(campcat_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(campcat_core PUBLIC
  ${OpenCV_LIBS}
  nlohmann_json::nlohmann_json
)

add_executable(campcat
  src/app/main.cpp
  src/app/template_capture_ui.cpp
  src/app/native_file_dialog.cpp
)
target_include_directories(campcat PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${portable_file_dialogs_SOURCE_DIR}
)
target_link_libraries(campcat PRIVATE campcat_core imgui_lib glfw OpenGL::GL)

if(APPLE)
  target_link_libraries(campcat PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
endif()

target_compile_definitions(campcat PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

add_executable(campcat_smoke
  src/tools/campcat_smoke.cpp
)
target_include_directories(campcat_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(campcat_smoke PRIVATE campcat_core)

enable_testing()
add_test(
  NAME ccat_smoke
  COMMAND campcat_smoke
          --config ${CMAKE_SOURCE_DIR}/config/default.json
          --cases-dir ${CMAKE_SOURCE_DIR}/tests/smoke/cases
)
set_tests_properties(ccat_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(
  NAME ccat_syntax
  COMMAND campcat_smoke
          --offline
          --config ${CMAKE_SOURCE_DIR}/config/default.json
          --cases-dir ${CMAKE_SOURCE_DIR}/tests/smoke/syntax
)
set_tests_properties(ccat_syntax PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(ccat_repl_test
  src/tools/ccat_repl_test.cpp
)
target_include_directories(ccat_repl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ccat_repl_test PRIVATE campcat_core)

add_test(NAME ccat_repl COMMAND ccat_repl_test)

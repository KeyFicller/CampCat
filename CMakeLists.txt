cmake_minimum_required(VERSION 3.20)
project(campcat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# OpenCV: install via Homebrew (`brew install opencv`), then configure with CMake.
# Apple Silicon hint if needed: -DOpenCV_DIR=/opt/homebrew/lib/cmake/opencv4
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

include(FetchContent)

# Shallow clones: full histories made imgui/json .git dirs ~250MB each and
# re-cloning missing glfw-src dominated configure time.
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
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
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.8
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(json)

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

# Core framework + Dear ImGui shell; game bundles under games/
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
  src/games/ccat_script/ccat_script_profile.cpp
  src/games/ccat_script/ccat_script_automation.cpp
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
)
target_include_directories(campcat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
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

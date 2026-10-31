cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
project(4DCAD LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(FD_BUILD_APP "Build the 4DCAD application (needs OpenGL)" ON)
option(FD_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- dependencies ---------------------------------------------------------------
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

# --- core (platform independent: no OpenGL / window) ------------------------------
add_library(fdcore STATIC
  src/primitives.cpp src/scene.cpp src/evaluator.cpp src/geodesic.cpp src/camera.cpp
  src/surface.cpp src/codegen.cpp src/examples.cpp src/library.cpp)
target_include_directories(fdcore PUBLIC src)
target_link_libraries(fdcore PUBLIC nlohmann_json::nlohmann_json)
if(MINGW)
  target_compile_definitions(fdcore PUBLIC _USE_MATH_DEFINES)
endif()
if(MSVC)
  target_compile_definitions(fdcore PUBLIC _USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_compile_options(fdcore PUBLIC /utf-8)
endif()

if(FD_BUILD_TESTS)
  enable_testing()
  add_executable(fdtests tests/test_core.cpp)
  target_link_libraries(fdtests PRIVATE fdcore)
  target_compile_definitions(fdtests PRIVATE FD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME core COMMAND fdtests)
endif()

if(FD_BUILD_APP)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip)
  FetchContent_MakeAvailable(glfw)

  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.6 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(imgui)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC glfw)

  set(APP_SOURCES src/main.cpp src/app.cpp src/ui.cpp src/renderer.cpp src/gl.cpp src/filedialog.cpp)
  if(WIN32)
    add_executable(4DCAD WIN32 ${APP_SOURCES})
    target_link_libraries(4DCAD PRIVATE comdlg32)
  else()
    add_executable(4DCAD ${APP_SOURCES})
  endif()
  target_link_libraries(4DCAD PRIVATE fdcore imgui glfw)
  if(FD_BUILD_TESTS)
    add_executable(fdapptests tests/test_app.cpp src/app.cpp src/renderer.cpp src/gl.cpp)
    target_link_libraries(fdapptests PRIVATE fdcore glfw)
    add_test(NAME app COMMAND fdapptests)
  endif()
  if(MINGW)
    target_link_options(4DCAD PRIVATE -static -static-libgcc -static-libstdc++ -s)
  endif()
endif()

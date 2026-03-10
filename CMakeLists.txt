cmake_minimum_required(VERSION 3.21)

project(veyoff-windows LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "veyoff-windows builds only on Windows")
endif()

set(VEYOFF_VERSION "0.0.0-dev" CACHE STRING "Version string embedded in the executable")
string(REGEX REPLACE "^v" "" VEYOFF_VERSION_CLEAN "${VEYOFF_VERSION}")
string(REGEX MATCH "^([0-9]+)\.([0-9]+)\.([0-9]+)(\.([0-9]+))?.*" VEYOFF_VERSION_MATCH "${VEYOFF_VERSION_CLEAN}")

if(CMAKE_MATCH_1)
  set(VEYOFF_VERSION_MAJOR "${CMAKE_MATCH_1}")
  set(VEYOFF_VERSION_MINOR "${CMAKE_MATCH_2}")
  set(VEYOFF_VERSION_PATCH "${CMAKE_MATCH_3}")
  if(CMAKE_MATCH_5)
    set(VEYOFF_VERSION_BUILD "${CMAKE_MATCH_5}")
  else()
    set(VEYOFF_VERSION_BUILD "0")
  endif()
else()
  set(VEYOFF_VERSION_MAJOR "0")
  set(VEYOFF_VERSION_MINOR "0")
  set(VEYOFF_VERSION_PATCH "0")
  set(VEYOFF_VERSION_BUILD "0")
endif()

set(VEYOFF_FILE_VERSION_STR "${VEYOFF_VERSION_MAJOR}.${VEYOFF_VERSION_MINOR}.${VEYOFF_VERSION_PATCH}.${VEYOFF_VERSION_BUILD}")
set(VEYOFF_ICON_PATH "${CMAKE_SOURCE_DIR}/docs/brand/logo.ico")
configure_file(
  "${CMAKE_SOURCE_DIR}/src/windows/veyoff-version.rc.in"
  "${CMAKE_BINARY_DIR}/veyoff-version.rc"
  @ONLY
)

add_executable(veyoff-windows
  src/windows/veyoff-windows.cpp
  "${CMAKE_BINARY_DIR}/veyoff-version.rc"
)

target_compile_features(veyoff-windows PRIVATE cxx_std_20)

target_compile_definitions(veyoff-windows PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_link_options(veyoff-windows PRIVATE
  -municode
)

target_link_libraries(veyoff-windows PRIVATE
  advapi32
  dwmapi
  gdi32
  iphlpapi
  shell32
  user32
  ws2_32
)

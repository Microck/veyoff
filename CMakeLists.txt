cmake_minimum_required(VERSION 3.21)

project(veyoff-windows LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "veyoff-windows builds only on Windows")
endif()

add_executable(veyoff-windows
  src/windows/veyoff-windows.cpp
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
  user32
  ws2_32
)

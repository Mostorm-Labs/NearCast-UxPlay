cmake_minimum_required(VERSION 3.20)
project(gst_texture_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
  gstreamer-1.0
  gstreamer-app-1.0
  gstreamer-video-1.0
  gstreamer-d3d11-1.0
)

add_executable(gst_texture_bridge
  src/gst_texture_bridge.cpp
)

target_compile_definitions(gst_texture_bridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(gst_texture_bridge PRIVATE
  PkgConfig::GSTREAMER
  d3d11
  dxgi
  ole32
)

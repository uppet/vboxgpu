cmake_minimum_required(VERSION 3.22)
project(vbox-gpu-bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

# LZ4 compression (used by host server and guest ICD for frame return)
add_library(lz4 STATIC common/third_party/lz4/lz4.c)
target_include_directories(lz4 PUBLIC common/third_party/lz4)

add_subdirectory(host)
add_subdirectory(guest_sim)
add_subdirectory(guest_vk_icd)
add_subdirectory(tests/dx11_triangle)
add_subdirectory(tests/dx11_depth_test)
add_subdirectory(tests/dx11_multi_blend)

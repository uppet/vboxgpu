cmake_minimum_required(VERSION 3.22)
project(vbox-gpu-bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

add_subdirectory(host)
add_subdirectory(guest_sim)
add_subdirectory(guest_vk_icd)
add_subdirectory(tests/dx11_triangle)
add_subdirectory(tests/dx11_depth_test)
add_subdirectory(tests/dx11_multi_blend)

set(ICD_SOURCES
    src/icd_dispatch.cpp
    src/icd_physical_device.cpp
)

add_library(vbox_vk_icd SHARED ${ICD_SOURCES})
target_link_libraries(vbox_vk_icd PRIVATE ws2_32 lz4)
target_compile_definitions(vbox_vk_icd PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_include_directories(vbox_vk_icd PRIVATE
    ${Vulkan_INCLUDE_DIRS}
)

# Output as vbox_vulkan.dll — loaded via VK_ICD_FILENAMES pointing to vbox_icd.json
set_target_properties(vbox_vk_icd PROPERTIES
    OUTPUT_NAME "vbox_vulkan"
)

# 32-bit: stdcall name decoration requires .def file for undecorated exports
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_sources(vbox_vk_icd PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vbox_vulkan.def")
endif()

# Copy ICD JSON to build directory, to be used with VK_ICD_FILENAMES
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/vbox_icd.json"
    "${CMAKE_CURRENT_BINARY_DIR}/vbox_icd.json"
    COPYONLY
)

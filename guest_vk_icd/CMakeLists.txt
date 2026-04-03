set(ICD_SOURCES
    src/icd_dispatch.cpp
    src/icd_physical_device.cpp
)

add_library(vbox_vk_icd SHARED ${ICD_SOURCES})
target_link_libraries(vbox_vk_icd PRIVATE ws2_32)
target_compile_definitions(vbox_vk_icd PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_include_directories(vbox_vk_icd PRIVATE
    ${Vulkan_INCLUDE_DIRS}
)

# Output as vulkan-1.dll for direct DXVK loading
set_target_properties(vbox_vk_icd PROPERTIES
    OUTPUT_NAME "vulkan-1"
)

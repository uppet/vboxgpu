set(HOST_SOURCES
    src/main.cpp
    src/vk_bootstrap.cpp
    src/vk_pipeline.cpp
    src/vk_renderer.cpp
)

add_executable(vbox_host WIN32 ${HOST_SOURCES})

target_include_directories(vbox_host PRIVATE src)
target_link_libraries(vbox_host PRIVATE Vulkan::Vulkan)

# --- Shader compilation via glslc ---
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found. Ensure Vulkan SDK is installed and VULKAN_SDK is set.")
endif()

set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

set(SHADERS
    ${SHADER_DIR}/triangle.vert
    ${SHADER_DIR}/triangle.frag
)

foreach(SHADER ${SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_FILE "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPV_FILE}
        COMMAND ${GLSLC} ${SHADER} -o ${SPV_FILE}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SPV_FILES ${SPV_FILE})
endforeach()

add_custom_target(shaders DEPENDS ${SPV_FILES})
add_dependencies(vbox_host shaders)

target_compile_definitions(vbox_host PRIVATE
    VK_USE_PLATFORM_WIN32_KHR
    SHADER_DIR="${SHADER_OUTPUT_DIR}"
)

# --- Command stream mode executable ---
set(CMDSTREAM_SOURCES
    src/main_cmdstream.cpp
    src/vk_bootstrap.cpp
    src/vn_decoder.cpp
    src/win_capture.cpp
)

add_executable(vbox_host_cmd ${CMDSTREAM_SOURCES})
target_include_directories(vbox_host_cmd PRIVATE src)
target_link_libraries(vbox_host_cmd PRIVATE Vulkan::Vulkan windowsapp)
add_dependencies(vbox_host_cmd shaders)
target_compile_definitions(vbox_host_cmd PRIVATE
    VK_USE_PLATFORM_WIN32_KHR
    SHADER_DIR="${SHADER_OUTPUT_DIR}"
)

# --- TCP server mode executable ---
set(SERVER_SOURCES
    src/main_server.cpp
    src/vk_bootstrap.cpp
    src/vn_decoder.cpp
    src/win_capture.cpp
)

add_executable(vbox_host_server ${SERVER_SOURCES})
target_include_directories(vbox_host_server PRIVATE src)
target_link_libraries(vbox_host_server PRIVATE Vulkan::Vulkan ws2_32 windowsapp lz4)
add_dependencies(vbox_host_server shaders)
target_compile_definitions(vbox_host_server PRIVATE
    VK_USE_PLATFORM_WIN32_KHR
    WINRT_LEAN_AND_MEAN
)

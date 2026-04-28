add_executable(capture_helper capture_helper.cpp)

# C++20 required for C++/WinRT headers
target_compile_features(capture_helper PRIVATE cxx_std_20)

target_link_libraries(capture_helper PRIVATE
    d3d11
    dxgi
    windowscodecs
    ole32
    runtimeobject
)

# Console subsystem (we write results to stdout/stderr)
set_target_properties(capture_helper PROPERTIES
    WIN32_EXECUTABLE FALSE
)

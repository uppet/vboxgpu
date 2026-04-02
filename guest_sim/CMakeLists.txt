set(GUEST_SIM_SOURCES
    src/main.cpp
)

add_executable(vbox_guest_sim ${GUEST_SIM_SOURCES})
target_link_libraries(vbox_guest_sim PRIVATE ws2_32)
target_compile_definitions(vbox_guest_sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

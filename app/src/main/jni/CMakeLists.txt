cmake_minimum_required(VERSION 3.22.1)
project(retro_shim)

add_library(retro_shim SHARED
    retro_shim.c
)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(retro_shim
    ${log-lib}
    ${android-lib}
)

# Needed for dlopen/dlsym of the libretro core .so at runtime.
target_link_libraries(retro_shim dl)

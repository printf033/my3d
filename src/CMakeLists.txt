add_library(interface_my3d INTERFACE)

find_package(assimp REQUIRED)

find_package(SDL2 REQUIRED)

set(FILAMENT_DIR "${CMAKE_SOURCE_DIR}/libs/filament/lib/x86_64")

target_link_libraries(interface_my3d INTERFACE
    c++
    ${FILAMENT_DIR}/libfilament.a
    ${FILAMENT_DIR}/libfilaflat.a
    ${FILAMENT_DIR}/libfilabridge.a
    ${FILAMENT_DIR}/libshaders.a
    ${FILAMENT_DIR}/libbackend.a
    ${FILAMENT_DIR}/libsmol-v.a
    ${FILAMENT_DIR}/libutils.a
    ${FILAMENT_DIR}/libzstd.a
    ${FILAMENT_DIR}/libbluegl.a
    ${FILAMENT_DIR}/libbluevk.a
    assimp::assimp
    SDL2::SDL2
)

target_include_directories(interface_my3d INTERFACE
    ${CMAKE_SOURCE_DIR}/src/mylog
    ${CMAKE_SOURCE_DIR}/libs/filament/include
    ${CMAKE_SOURCE_DIR}/libs/stb
)

target_compile_features(interface_my3d INTERFACE
    cxx_std_20
)

target_compile_options(interface_my3d INTERFACE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall -Wextra -fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=>
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>
    $<$<AND:$<CONFIG:Debug>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-fsanitize=address -fno-omit-frame-pointer>
)

target_link_options(interface_my3d INTERFACE
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>
    $<$<AND:$<CONFIG:Debug>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-fsanitize=address>
)


target_compile_definitions(interface_my3d INTERFACE
    $<$<PLATFORM_ID:Windows>:VK_USE_PLATFORM_WIN32_KHR>
    $<$<PLATFORM_ID:Darwin>:VK_USE_PLATFORM_METAL_EXT>
    $<$<AND:$<PLATFORM_ID:Linux>,$<NOT:$<PLATFORM_ID:Android>>>:VK_USE_PLATFORM_WAYLAND_KHR>
    SDL_VIDEO_DRIVER_WAYLAND
    STB_IMAGE_IMPLEMENTATION
)

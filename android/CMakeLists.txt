cmake_minimum_required(VERSION 3.22)

project(my3d_android LANGUAGES C CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "Configure this project with the Android NDK CMake toolchain.")
endif()

set(MY3D_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "my3d source root")
set(MY3D_FILAMENT_ROOT "" CACHE PATH "Filament v1.77.2 Android archive root")
set(MY3D_ASSIMP_SOURCE_DIR "" CACHE PATH "Assimp v6.0.5 source tree")

if(NOT EXISTS "${MY3D_FILAMENT_ROOT}/include/filament/Engine.h")
    message(FATAL_ERROR "Set MY3D_FILAMENT_ROOT to the extracted Filament Android package.")
endif()
if(NOT EXISTS "${MY3D_FILAMENT_ROOT}/lib/${ANDROID_ABI}/libfilament.a")
    message(FATAL_ERROR "Filament has no ${ANDROID_ABI} libraries under MY3D_FILAMENT_ROOT.")
endif()
if(NOT EXISTS "${MY3D_ASSIMP_SOURCE_DIR}/include/assimp/Importer.hpp")
    message(FATAL_ERROR "Set MY3D_ASSIMP_SOURCE_DIR to the Assimp v6.0.5 source tree.")
endif()

set(NATIVE_APP_GLUE "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
if(NOT EXISTS "${NATIVE_APP_GLUE}/android_native_app_glue.c")
    message(FATAL_ERROR "NDK native_app_glue was not found at ${NATIVE_APP_GLUE}.")
endif()

set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory("${MY3D_ASSIMP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/assimp" EXCLUDE_FROM_ALL)

add_library(android_native_app_glue STATIC
    "${NATIVE_APP_GLUE}/android_native_app_glue.c")
target_include_directories(android_native_app_glue PUBLIC "${NATIVE_APP_GLUE}")

add_library(my3d_android SHARED
    app/src/main/cpp/android_main.cpp
    "${MY3D_ROOT}/src/scene/world.cpp"
    "${MY3D_ROOT}/src/render/toolbox.cpp"
    "${MY3D_ROOT}/src/render/camera_controller.cpp"
    "${MY3D_ROOT}/src/render/scene_builder.cpp"
    "${MY3D_ROOT}/src/render/animation_driver.cpp"
    "${MY3D_ROOT}/src/asset/asset_scene.cpp"
    "${MY3D_ROOT}/src/asset/animation.cpp"
    "${MY3D_ROOT}/src/asset/ibl.cpp"
    "${MY3D_ROOT}/src/asset/importer.cpp"
    "${MY3D_ROOT}/src/backend/filament/filament_renderer.cpp")

target_compile_features(my3d_android PRIVATE cxx_std_20)
target_compile_definitions(my3d_android PRIVATE STB_IMAGE_IMPLEMENTATION)
target_compile_options(my3d_android PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unknown-pragmas
    -fmacro-prefix-map=${MY3D_ROOT}/=)
target_include_directories(my3d_android PRIVATE
    "${MY3D_ROOT}/src"
    "${MY3D_ROOT}/libs"
    "${MY3D_FILAMENT_ROOT}/include"
    "${MY3D_ASSIMP_SOURCE_DIR}/include")

set(FILAMENT_LIB_DIR "${MY3D_FILAMENT_ROOT}/lib/${ANDROID_ABI}")
set(FILAMENT_LIBRARIES
    filament filaflat filabridge shaders backend smol-v
    image ktxreader basis_transcoder utils zstd bluevk abseil stb)
foreach(library IN LISTS FILAMENT_LIBRARIES)
    if(NOT EXISTS "${FILAMENT_LIB_DIR}/lib${library}.a")
        message(FATAL_ERROR "Missing Filament Android library: ${FILAMENT_LIB_DIR}/lib${library}.a")
    endif()
    target_link_libraries(my3d_android PRIVATE "${FILAMENT_LIB_DIR}/lib${library}.a")
endforeach()

find_library(ANDROID_LIB android REQUIRED)
find_library(LOG_LIB log REQUIRED)
find_library(VULKAN_LIB vulkan REQUIRED)
find_library(GLESV3_LIB GLESv3 REQUIRED)
find_library(EGL_LIB EGL REQUIRED)
find_library(Z_LIB z REQUIRED)
target_link_libraries(my3d_android PRIVATE
    android_native_app_glue
    assimp::assimp
    ${ANDROID_LIB}
    ${LOG_LIB}
    ${VULKAN_LIB}
    ${GLESV3_LIB}
    ${EGL_LIB}
    ${Z_LIB}
    dl m)

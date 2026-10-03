cmake_minimum_required(VERSION 3.22.1)
project(prueba_vk13 LANGUAGES C CXX)

set(ADRENOTOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../../out/libadrenotools" CACHE PATH "libadrenotools")
add_subdirectory("${ADRENOTOOLS_DIR}" adrenotools)

add_library(native_app_glue STATIC "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(prueba_vk13 SHARED main.cpp)
target_link_libraries(prueba_vk13 PRIVATE native_app_glue adrenotools android log dl)
target_link_options(prueba_vk13 PRIVATE "-u" "ANativeActivity_onCreate")
add_dependencies(prueba_vk13 main_hook hook_impl)

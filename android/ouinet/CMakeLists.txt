cmake_minimum_required(VERSION 3.7.0)

project(native-lib)

# Place compiled executables in the gradle Assets directory.
# If there is any better way of finding this, such as getting it out of gradle
# in some way, I'm all ears.
get_filename_component(RUNTIME_DIR_1 ${CMAKE_LIBRARY_OUTPUT_DIRECTORY} DIRECTORY)
get_filename_component(RUNTIME_DIR_2 ${RUNTIME_DIR_1} DIRECTORY)
get_filename_component(RUNTIME_DIR_3 ${RUNTIME_DIR_2} DIRECTORY)
get_filename_component(RUNTIME_DIR_4 ${RUNTIME_DIR_3} DIRECTORY)
get_filename_component(RUNTIME_DIR_VARIANT ${RUNTIME_DIR_2} NAME)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR_4}/library_assets/${RUNTIME_DIR_VARIANT}/out")

# TODO: This is to silent warnings from using unqualified std::move and std::forward.
# The internet says we should used qualified ones so we should probably start doing that.
add_compile_options(-Wno-unqualified-std-cast-call)

add_subdirectory(../.. libouinet)

add_library(native-lib SHARED src/main/cpp/native-lib.cpp )

if(WITH_ASAN)
    message("Using asan")

    # For asan we need to copy some files from the NDK into our APK.
    get_filename_component(HINT_PATH ${ANDROID_C_COMPILER} DIRECTORY)
    set(ARCH_STR ${CMAKE_ANDROID_ARCH})
    if ("${CMAKE_ANDROID_ARCH_ABI}" STREQUAL "arm64-v8a")
        set(ARCH_STR "aarch64")
    elseif ("${CMAKE_ANDROID_ARCH_ABI}" STREQUAL "armeabi")
        set(ARCH_STR "arm")
    elseif ("${CMAKE_ANDROID_ARCH_ABI}" STREQUAL "armeabi-v7a")
        set(ARCH_STR "arm")
    elseif ("${CMAKE_ANDROID_ARCH_ABI}" STREQUAL "x86")
        set(ARCH_STR "i686")
    elseif ("${CMAKE_ANDROID_ARCH_ABI}" STREQUAL "x86_64")
        set(ARCH_STR "x86_64")
    endif()

    target_compile_options(native-lib PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(native-lib PUBLIC -fsanitize=address)

    # Grab libclang_rt.asan-${ARCH_STR}-android.so from the NDK.
    file(GLOB ASAN_GLOB "${HINT_PATH}/../lib/clang/*/lib/linux")
    find_file(ASAN
            NAMES libclang_rt.asan-${ARCH_STR}-android.so
            PATHS ${ASAN_GLOB})
    get_filename_component(ASAN_NAME ${ASAN} NAME)
    set(ASAN_NAME ${CMAKE_SOURCE_DIR}/src/debug/jniLibs/${CMAKE_ANDROID_ARCH_ABI}/${ASAN_NAME})
    add_custom_command(
            TARGET native-lib PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${ASAN} ${ASAN_NAME})

    # Grab the asan wrapper script from the NDK.
    find_file(WRAP
            NAMES asan.sh
            HINTS ${HINT_PATH}/../../../../../wrap.sh)
    set(WRAP_NAME ${CMAKE_SOURCE_DIR}/src/debug/resources/lib/${CMAKE_ANDROID_ARCH_ABI}/wrap.sh)
    add_custom_command(
            TARGET native-lib PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${WRAP} ${WRAP_NAME})
endif()

target_link_libraries(native-lib
    PRIVATE
        ouinet::client
        ${android_log}
)

# Add libc++_shared.so to the aar file
set(LIBCPP_SHARED_PATH
    ${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/libc++_shared.so
)
add_custom_command(
    OUTPUT ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libc++_shared.so
    COMMAND ${CMAKE_COMMAND} -E copy ${LIBCPP_SHARED_PATH} ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)
add_custom_target(libcpp_shared ALL DEPENDS ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libc++_shared.so)

add_dependencies(native-lib libcpp_shared)
if(WITH_DEPRECATED)
    add_dependencies(native-lib obfs4proxy)
endif()

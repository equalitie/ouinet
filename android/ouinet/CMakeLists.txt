cmake_minimum_required(VERSION 3.7.0)

# Place compiled executables in the gradle Assets directory.
# If there is any better way of finding this, such as getting it out of gradle
# in some way, I'm all ears.
get_filename_component(RUNTIME_DIR_1 ${CMAKE_LIBRARY_OUTPUT_DIRECTORY} DIRECTORY)
get_filename_component(RUNTIME_DIR_2 ${RUNTIME_DIR_1} DIRECTORY)
get_filename_component(RUNTIME_DIR_3 ${RUNTIME_DIR_2} DIRECTORY)
get_filename_component(RUNTIME_DIR_4 ${RUNTIME_DIR_3} DIRECTORY)
get_filename_component(RUNTIME_DIR_VARIANT ${RUNTIME_DIR_2} NAME)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR_4}/library_assets/${RUNTIME_DIR_VARIANT}/out")


add_subdirectory(../.. libouinet)

add_library(native-lib SHARED src/main/cpp/native-lib.cpp )
target_link_libraries(native-lib
    PRIVATE
        ouinet::client
        ${android_log}
)

# Add libc++_shared.so to the aar file
get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
get_filename_component(COMPILER_TOOLCHAIN_PREFIX ${_CMAKE_TOOLCHAIN_PREFIX} NAME)
string(REGEX REPLACE "-$" "" COMPILER_HOSTTRIPLE ${COMPILER_TOOLCHAIN_PREFIX})
set(LIBCPP_SHARED_PATH
    ${COMPILER_DIR}/../sysroot/usr/lib/${COMPILER_HOSTTRIPLE}/libc++_shared.so
)
add_custom_command(
    OUTPUT ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libc++_shared.so
    COMMAND ${CMAKE_COMMAND} -E copy ${LIBCPP_SHARED_PATH} ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)
add_custom_target(libcpp_shared ALL DEPENDS ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libc++_shared.so)

add_dependencies(native-lib libcpp_shared)
add_dependencies(native-lib obfs4proxy)

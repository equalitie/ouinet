cmake_minimum_required (VERSION 3.2)
project (ouinet-iOS)
enable_testing()

if(DEFINED CMAKE_OBJC_COMPILER)
  # Environment variables are always preserved.
  set(ENV{_CMAKE_OBJC_COMPILER} "${CMAKE_OBJC_COMPILER}")
elseif(DEFINED ENV{_CMAKE_OBJC_COMPILER})
  set(CMAKE_OJBC_COMPILER "$ENV{_CMAKE_OBJC_COMPILER}")
elseif(NOT DEFINED CMAKE_OBJC_COMPILER)
  execute_process(COMMAND xcrun -sdk ${CMAKE_OSX_SYSROOT_INT} -find clang
          OUTPUT_VARIABLE CMAKE_OBJC_COMPILER
          ERROR_QUIET
          OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
MESSAGE( STATUS "CMAKE_OBJC_COMPILER: " ${CMAKE_OBJC_COMPILER} )

if(DEFINED CMAKE_OBJCXX_COMPILER)
  # Environment variables are always preserved.
  set(ENV{_CMAKE_OBJCXX_COMPILER} "${CMAKE_OBJCXX_COMPILER}")
elseif(DEFINED ENV{_CMAKE_OBJCXX_COMPILER})
  set(CMAKE_OJBC_COMPILER "$ENV{_CMAKE_OBJCXX_COMPILER}")
elseif(NOT DEFINED CMAKE_OBJCXX_COMPILER)
  execute_process(COMMAND xcrun -sdk ${CMAKE_OSX_SYSROOT_INT} -find clang++
          OUTPUT_VARIABLE CMAKE_OBJCXX_COMPILER
          ERROR_QUIET
          OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
MESSAGE( STATUS "CMAKE_OBJCXX_COMPILER: " ${CMAKE_OBJCXX_COMPILER} )

enable_language(CXX)
enable_language(OBJC)

MESSAGE( STATUS "CMAKE_CXX_FLAGS: " ${CMAKE_CXX_FLAGS} )
MESSAGE( STATUS "CMAKE_OBJC_FLAGS: " ${CMAKE_OBJC_FLAGS} )

# Add some sanitary checks that the toolchain is actually working!
include(CheckCXXSymbolExists)
check_cxx_symbol_exists(kqueue sys/event.h HAVE_KQUEUE)
if(NOT HAVE_KQUEUE)
  message(STATUS "kqueue NOT found!")
else()
  message(STATUS "kqueue found!")
endif()

find_library(APPKIT_LIBRARY AppKit)
if (NOT APPKIT_LIBRARY)
  message(STATUS "AppKit.framework NOT found!")
else()
  message(STATUS "AppKit.framework found! ${APPKIT_LIBRARY}")
endif()

find_library(FOUNDATION_LIBRARY Foundation)
if (NOT FOUNDATION_LIBRARY)
  message(STATUS "Foundation.framework NOT found!")
else()
  message(STATUS "Foundation.framework found! ${FOUNDATION_LIBRARY}")
endif()

find_library(UIKIT_LIBRARY UIKit)
if (NOT UIKIT_LIBRARY)
  message(STATUS "UIKit.framework NOT found!")
else()
  message(STATUS "UIKit.framework found! ${UIKIT_LIBRARY}")
endif()

# Hook up XCTest for the supported plaforms (all but WatchOS)
if(NOT PLATFORM MATCHES ".*WATCHOS.*")
  # Use the standard find_package, broken between 3.14.0 and 3.14.4 at least for XCtest...
  find_package(XCTest)
  # Fallback: Try to find XCtest as host package via toochain macro (should always work)
  find_host_package(XCTest REQUIRED)
endif()


# Includes
include_directories(${ouinet_DIR}/src ${ouinet_DIR}/include)

# Make sure try_compile() works
include(CheckTypeSize)
check_type_size(time_t SIZEOF_TIME_T)

# Source files
set(SOURCES
  ${ouinet_DIR}/src/native-lib.cpp
  ${ouinet_DIR}/src/ouinet/OuinetClient.mm
  ${ouinet_DIR}/src/ouinet/OuinetConfig.mm
)

# Headers
set(HEADERS
  ${ouinet_DIR}/src/native-lib.hpp
  ${ouinet_DIR}/include/Ouinet.h
  ${ouinet_DIR}/include/ouinet/OuinetClient.h
  ${ouinet_DIR}/include/ouinet/OuinetConfig.h
)

# Library
add_library (ouinet SHARED ${SOURCES} ${HEADERS})
target_link_libraries(ouinet
  PRIVATE
      ouinet::client
      ${FOUNDATION_LIBRARY}
)
target_compile_definitions(ouinet PUBLIC IS_BUILDING_SHARED)
message(STATUS "Building framework bundle...")

# Debug symbols set in XCode project
set_xcode_property(ouinet GCC_GENERATE_DEBUGGING_SYMBOLS YES "All")

install(TARGETS ouinet
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib/static)

install (FILES ${HEADERS} DESTINATION include)

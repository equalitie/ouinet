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

set(OUINET_IOS_SOURCE ${ouinet-ios_DIR}/src)

# Includes
include_directories(${OUINET_IOS_SOURCE})

# Make sure try_compile() works
include(CheckTypeSize)
check_type_size(time_t SIZEOF_TIME_T)

# Source files
set(SOURCES
  ${OUINET_IOS_SOURCE}/native-lib.cpp
  ${OUINET_IOS_SOURCE}/ouinet/OuinetClient.mm
  ${OUINET_IOS_SOURCE}/ouinet/OuinetConfig.mm
)

# Headers
set(HEADERS
  ${OUINET_IOS_SOURCE}/native-lib.hpp
  ${OUINET_IOS_SOURCE}/Ouinet.h
  ${OUINET_IOS_SOURCE}/ouinet/OuinetClient.h
  ${OUINET_IOS_SOURCE}/ouinet/OuinetConfig.h
)

# Library
#if(BUILD_SHARED)
add_library (ouinet-ios SHARED ${SOURCES} ${HEADERS})
#target_link_libraries(ouinet-ios ${FOUNDATION_LIBRARY})
target_link_libraries(ouinet-ios
  PRIVATE
      ouinet::client
      ${FOUNDATION_LIBRARY}
)
target_compile_definitions(ouinet-ios PUBLIC IS_BUILDING_SHARED)
message(STATUS "Building iOS framework bundle...")
set_target_properties(ouinet-ios PROPERTIES
  FRAMEWORK TRUE
  FRAMEWORK_VERSION C
  MACOSX_FRAMEWORK_IDENTIFIER ie.equalit.ouinet-ios
  # "current version" in semantic format in Mach-O binary file
  VERSION 16.4.0
  # "compatibility version" in semantic format in Mach-O binary file
  SOVERSION 1.0.0
  PUBLIC_HEADER Ouinet.h
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
)

# Executable
if(PLATFORM MATCHES "MAC.*")
  set(APP_NAME TestApp)
  add_executable (${APP_NAME} MACOSX_BUNDLE main.cpp)
  set_target_properties(${APP_NAME} PROPERTIES
          BUNDLE True
          MACOSX_BUNDLE_GUI_IDENTIFIER equalit.ie.ouinet
          MACOSX_BUNDLE_BUNDLE_NAME ouinet
          MACOSX_BUNDLE_BUNDLE_VERSION "0.1"
          MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
          )
  # Link the library with the executable
  target_link_libraries(${APP_NAME} ouinet-ios)
endif()

# Debug symbols set in XCode project
set_xcode_property(ouinet-ios GCC_GENERATE_DEBUGGING_SYMBOLS YES "All")

# Installation
if(PLATFORM MATCHES "MAC.*")
  install(TARGETS ${APP_NAME}
          BUNDLE DESTINATION . COMPONENT Runtime
          RUNTIME DESTINATION bin COMPONENT Runtime
          LIBRARY DESTINATION lib
          ARCHIVE DESTINATION lib/static)

  # Note Mac specific extension .app
  set(APPS "\${CMAKE_INSTALL_PREFIX}/${APP_NAME}.app")

  # Directories to look for dependencies
  set(DIRS ${CMAKE_BINARY_DIR})

  install(CODE "include(BundleUtilities)
    fixup_bundle(\"${APPS}\" \"\" \"${DIRS}\")")

  set(CPACK_GENERATOR "DRAGNDROP")
  include(CPack)
else()
  install(TARGETS ouinet-ios
          LIBRARY DESTINATION lib
          ARCHIVE DESTINATION lib/static
          FRAMEWORK DESTINATION framework)
endif()
install (FILES ${HEADERS} DESTINATION include)

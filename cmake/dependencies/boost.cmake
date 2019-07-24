set(BOOST_VERSION 1.67)
set(BOOST_COMPONENTS
    context
    coroutine
    date_time
    filesystem
    iostreams
    program_options
    regex
    system
    thread
    unit_test_framework
)

find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS ${BOOST_COMPONENTS})
find_package(Threads REQUIRED)


if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    # Boost.Beast has a patch that uses the ANDROID definition
    # instead of the implicit __ANDROID__ one.
    # https://github.com/boostorg/beast/blob/44f37d1a11d/include/boost/beast/core/impl/file_posix.ipp#L27
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS "ANDROID"
    )
endif()


# gcc 8 spits out warnings from Boost.Mpl about unnecessary parentheses
# https://github.com/CauldronDevelopmentLLC/cbang/issues/26
# TODO: Perhaps do a check for Boost and gcc version before adding this flag?
set_target_properties(Boost::boost PROPERTIES
    INTERFACE_COMPILE_OPTIONS "-Wno-parentheses"
)


# Asio can be used as a header-only library _only_ if it is not used along any
# shared library boundaries. In any program using asio across a library
# boundary, both the library and the program must link asio as a shared library
# instead. Boost does not ship this library, so we need to create it.

add_library(boost_asio SHARED "${CMAKE_CURRENT_SOURCE_DIR}/lib/asio.cpp")
add_library(Boost::asio ALIAS boost_asio)
target_link_libraries(boost_asio
    PUBLIC
        Boost::boost
        Boost::coroutine
        Threads::Threads
    PRIVATE
        Boost::system
)
target_compile_definitions(boost_asio
    PUBLIC
        -DBOOST_ASIO_DYN_LINK
        # For some reason we need to define both of these
        -DBOOST_COROUTINES_NO_DEPRECATION_WARNING
        -DBOOST_COROUTINE_NO_DEPRECATION_WARNING
)

add_library(boost_asio_ssl SHARED "${CMAKE_CURRENT_SOURCE_DIR}/lib/asio_ssl.cpp")
add_library(Boost::asio_ssl ALIAS boost_asio_ssl)
target_link_libraries(boost_asio_ssl
    PRIVATE OpenSSL::SSL
    PUBLIC Boost::asio
)


# FindBoost.cmake doesn't define targets for newer versions of boost.
# Let's emulate it instead.
if(NOT ${_Boost_IMPORTED_TARGETS})
    foreach(component ${BOOST_COMPONENTS})
        find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS ${component})
        add_library(boost_${component} INTERFACE)
        add_library(Boost::${component} ALIAS boost_${component})
        target_include_directories(boost_${component} INTERFACE ${Boost_INCLUDE_DIRS})
        target_link_libraries(boost_${component} INTERFACE ${Boost_LIBRARIES})
    endforeach()
endif()


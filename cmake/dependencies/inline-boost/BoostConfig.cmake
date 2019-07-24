include(${CMAKE_CURRENT_LIST_DIR}/boost-dependencies.cmake)

function(_boost_library_filename component output_var)
    set(${output_var} "${BUILT_BOOST_LIBRARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}boost_${component}${CMAKE_STATIC_LIBRARY_SUFFIX}" PARENT_SCOPE)
endfunction(_boost_library_filename)


set(Boost_VERSION ${BUILT_BOOST_VERSION})
set(Boost_LIB_VERSION "")
string(REGEX MATCH "([0-9]+)(\\.([0-9]+)(\\.([0-9]+))?)?" _version_parts "${BUILT_BOOST_VERSION}")
set(Boost_MAJOR_VERSION "${_version_parts_1}")
set(Boost_MINOR_VERSION "${_version_parts_3}")
set(Boost_SUBMINOR_VERSION "${_version_parts_5}")

set(Boost_INCLUDE_DIR ${BUILT_BOOST_INCLUDE_DIR} CACHE PATH "")
set(Boost_INCLUDE_DIRS ${Boost_INCLUDE_DIR})
set(Boost_LIBRARY_DIRS ${BUILT_BOOST_LIBRARY_DIR})

file(MAKE_DIRECTORY ${Boost_INCLUDE_DIR})

if (NOT TARGET Boost::boost)
    add_library(Boost::boost INTERFACE IMPORTED)
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}"
    )
    add_dependencies(Boost::boost built_boost)

    add_library(Boost::diagnostic_definitions INTERFACE IMPORTED)
    add_library(Boost::disable_autolinking INTERFACE IMPORTED)
    add_library(Boost::dynamic_linking INTERFACE IMPORTED)
endif()



foreach (component ${BUILT_BOOST_COMPONENTS})
    string(TOUPPER ${component} UPPERCOMPONENT)

    set(Boost_${UPPERCOMPONENT}_FOUND ON)
    _boost_library_filename(${component} Boost_${UPPERCOMPONENT}_LIBRARY)

    if (NOT TARGET Boost::${component})
        add_library(Boost::${component} UNKNOWN IMPORTED)
        set_target_properties(Boost::${component} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}"
            IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
            IMPORTED_LOCATION ${Boost_${UPPERCOMPONENT}_LIBRARY}
        )
        add_dependencies(Boost::${component} built_boost)
        if (NOT "${_static_Boost_${UPPERCOMPONENT}_DEPENDENCIES}" STREQUAL "")
            set(_Boost_${UPPERCOMPONENT}_FULL_DEPENDENCIES )
            foreach (dependency ${_static_Boost_${UPPERCOMPONENT}_DEPENDENCIES})
                _boost_library_filename(${dependency} dependency_filename)
                set(_Boost_${UPPERCOMPONENT}_FULL_DEPENDENCIES
                    ${_Boost_${UPPERCOMPONENT}_FULL_DEPENDENCIES}
                    ${dependency_filename}
                )
            endforeach()
            set_target_properties(Boost::${component} PROPERTIES
                INTERFACE_LINK_LIBRARIES "${_Boost_${UPPERCOMPONENT}_FULL_DEPENDENCIES}"
            )
        endif()

        # Make sure that any packages that link against the raw library file still
        # get the dependency order correct.
        add_custom_command(OUTPUT ${Boost_${UPPERCOMPONENT}_LIBRARY} COMMAND "" DEPENDS built_boost)
    endif()
endforeach()


set(Boost_LIBRARIES )
foreach (component ${Boost_FIND_COMPONENTS})
    string(TOUPPER ${component} UPPERCOMPONENT)

    if (NOT Boost_${UPPERCOMPONENT}_FOUND)
        if (${Boost_FIND_REQUIRED})
            message(SEND_ERROR "Unable to find the requested Boost library: ${component}")
        elseif (NOT ${Boost_FIND_QUIETLY})
            message(STATUS "Could NOT find Boost library: ${component}")
        endif()
    else()
        set(Boost_LIBRARIES ${Boost_LIBRARIES}
            ${Boost_${UPPERCOMPONENT}_LIBRARY}
            ${Boost_${UPPERCOMPONENT}_FULL_DEPENDENCIES}
        )
    endif()
endforeach()

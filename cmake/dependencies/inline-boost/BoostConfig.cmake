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
        add_library(boost_${component} UNKNOWN IMPORTED)
        set_target_properties(boost_${component} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}"
            IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
            IMPORTED_LOCATION ${Boost_${UPPERCOMPONENT}_LIBRARY}
        )

        add_library(boost_${component}_ INTERFACE)
        add_library(Boost::${component} ALIAS boost_${component}_)
        add_dependencies(boost_${component}_ built_boost)

        _static_Boost_recursive_dependencies(${component} dependencies)
        _static_Boost_external_libraries(${component} libraries)
        set(_Boost_${UPPERCOMPONENT}_LINK_LIBRARIES boost_${component})
        foreach (dependency ${dependencies})
            _boost_library_filename(${dependency} dependency_filename)
            list(APPEND _Boost_${UPPERCOMPONENT}_LINK_LIBRARIES ${dependency_filename})
        endforeach()
        foreach (library ${libraries})
            list(APPEND _Boost_${UPPERCOMPONENT}_LINK_LIBRARIES ${library})
        endforeach()
        set_target_properties(boost_${component}_ PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_Boost_${UPPERCOMPONENT}_LINK_LIBRARIES}"
        )
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

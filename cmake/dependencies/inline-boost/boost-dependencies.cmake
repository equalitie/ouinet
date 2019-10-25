set(_static_Boost_CHRONO_DEPENDENCIES system)
set(_static_Boost_CONTEXT_DEPENDENCIES thread chrono system date_time)
set(_static_Boost_COROUTINE_DEPENDENCIES context system)
set(_static_Boost_FIBER_DEPENDENCIES context thread chrono system date_time)
set(_static_Boost_FILESYSTEM_DEPENDENCIES system)
set(_static_Boost_IOSTREAMS_DEPENDENCIES regex)
set(_static_Boost_LOG_DEPENDENCIES date_time log_setup system filesystem thread regex chrono atomic)
set(_static_Boost_MATH_DEPENDENCIES math_c99 math_c99f math_c99l math_tr1 math_tr1f math_tr1l atomic)
set(_static_Boost_MPI_DEPENDENCIES serialization)
set(_static_Boost_RANDOM_DEPENDENCIES system)
set(_static_Boost_THREAD_DEPENDENCIES chrono system date_time atomic)
set(_static_Boost_TIMER_DEPENDENCIES chrono system)
set(_static_Boost_WAVE_DEPENDENCIES filesystem system serialization thread chrono date_time atomic)
set(_static_Boost_WSERIALIZATION_DEPENDENCIES serialization)


# Compute the recursive dependencies for a given boost module
function(_static_Boost_recursive_dependencies components outputvar)
    set(_unprocessed ${components})
    set(_processed )

    while (_unprocessed)
        list(GET _unprocessed 0 _item)
        list(REMOVE_AT _unprocessed 0)
        list(APPEND _processed ${_item})

        string(TOUPPER ${_item} upperitem)
        foreach(dependency ${_static_Boost_${upperitem}_DEPENDENCIES})
            if (NOT ("${dependency}" IN_LIST _unprocessed OR "${dependency}" IN_LIST _processed))
                list(APPEND _unprocessed ${dependency})
            endif()
        endforeach()
    endwhile()
    set(${outputvar} ${_processed} PARENT_SCOPE)
endfunction()


set(_static_Boost_IOSTREAMS_EXTERNAL_LIBRARIES z)


# List the external library dependencies for a given boost module
function(_static_Boost_external_libraries component outputvar)
    string(TOUPPER ${component} UPPERCOMPONENT)
    set(${outputvar} ${_static_Boost_${UPPERCOMPONENT}_EXTERNAL_LIBRARIES} PARENT_SCOPE)
endfunction()

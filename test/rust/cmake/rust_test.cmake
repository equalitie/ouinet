# Add a rust integration test with the specified name to the project
function(add_rust_test name)
    # TODO: consider making this an argument
    set(output_dir "${CMAKE_CURRENT_BINARY_DIR}")

    set(manifest_path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml")
    set(target_dir "${CMAKE_CURRENT_BINARY_DIR}/rust/target")

    set(include_dirs
        "$<JOIN:$<TARGET_PROPERTY:ouinet_client,INTERFACE_INCLUDE_DIRECTORIES>,$<COMMA>>"
    )

    set(libs ouinet_client ouinet_injector ouinet_common ouinet_asio)
    list(JOIN libs "," libs_str)

    set(lib_dirs ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    list(JOIN lib_dirs "," lib_dirs)

    set(cargo_output_file "${CMAKE_CURRENT_BINARY_DIR}/rust/${name}.output.json")

    # Check the test for errors
    add_custom_target(
        "_check_${name}"
        COMMENT "Checking ${name}"
        COMMAND
            ${CMAKE_COMMAND} -E env
                INCLUDE_DIRS=${include_dirs}
                LIB_DIRS=${lib_dirs}
                LIBS=${libs_str}
            cargo build --test ${name}
                        --manifest-path ${manifest_path}
                        --target-dir ${target_dir}
        VERBATIM
    )

    add_dependencies("_check_${name}" ${libs})

    # Build the test and write the machine-processable output to a file
    add_custom_target(
        "_build_${name}"
        COMMENT "Building ${name}"
        COMMAND
            ${CMAKE_COMMAND} -E env
                INCLUDE_DIRS=${include_dirs}
                LIB_DIRS=${lib_dirs}
                LIBS=${libs_str}
            cargo build --test ${name}
                        --manifest-path ${manifest_path}
                        --target-dir ${target_dir}
                        --message-format json
            > ${cargo_output_file}
        VERBATIM
    )

    add_dependencies("_build_${name}" "_check_${name}")

    # Collect the test binaries and copy them to the main test directory
    add_custom_target(
        ${name}
        COMMENT "Collecting ${name}"
        COMMAND
            ${CMAKE_COMMAND}
                -DCARGO_OUTPUT=${cargo_output_file}
                -DOUTPUT_DIR=${output_dir}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/rust/cmake/collect_test_binaries.cmake"
    )

    add_dependencies(${name} "_build_${name}")
endfunction()

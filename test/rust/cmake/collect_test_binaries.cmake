# Parse JSON formatted output of cargo build from the file at `CARGO_OUTPUT`, extract all
# executables from it and copy them to the `OUTPUT_DIR`, stripping the hash from their filenames.

file(READ "${CARGO_OUTPUT}" content)
string(REPLACE "\n" ";" lines "${content}")

foreach(line IN LISTS lines)
    if(line STREQUAL "")
        continue()
    endif()

    # Process only records with "reason": "compiler-artifact"
    string(JSON reason GET "${line}" "reason")
    if(NOT reason STREQUAL "compiler-artifact")
        continue()
    endif()

    # Process only tests
    string(JSON is_test GET "${line}" "profile" "test")
    if(NOT is_test)
        continue()
    endif()

    # Extract the executable path
    string(JSON executable GET "${line}" "executable")
    if(executable STREQUAL "")
        continue()
    endif()

    cmake_path(GET executable STEM exe_stem)
    cmake_path(GET executable EXTENSION exe_ext)

    # Strip the hash from the test filename
    string(REGEX REPLACE "-[0-9a-f]+$" "" exe_stem_clean "${exe_stem}")

    # Copy the test binary to the output dir
    file(COPY_FILE "${executable}" "${OUTPUT_DIR}/${exe_stem_clean}${exe_ext}")
endforeach()

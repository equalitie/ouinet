if (OUINET_HARDCODE_CA_CERTS)
    # This cmake file downloads CA certificates, converts them to a C++ header and
    # cpp file and creates a library target for linking.
    
    set(OUINET_CA_CERTS_DIR ${CMAKE_BINARY_DIR}/cacert)
    set(OUINET_CA_CERTS_PEM "${OUINET_CA_CERTS_DIR}/cacert.pem")
    
    file(DOWNLOAD
        https://curl.se/ca/cacert.pem
        ${OUINET_CA_CERTS_PEM}
        # From Feb 26 2026
        EXPECTED_HASH SHA256=f1407d974c5ed87d544bd931a278232e13925177e239fca370619aba63c757b4
        SHOW_PROGRESS
        STATUS status
    )
    
    list(GET status 0 status_code)
    
    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "Downloading CA certificates failed")
    endif()
    
    set(OUINET_CA_CERTS_CONVERTER ${OUINET_CA_CERTS_DIR}/convert.sh)
    set(OUINET_CA_CERTS_H "${OUINET_CA_CERTS_DIR}/cacert.pem.h")
    set(OUINET_CA_CERTS_CPP "${OUINET_CA_CERTS_DIR}/cacert.pem.cpp")
    
    file(WRITE ${OUINET_CA_CERTS_CONVERTER} [=[
        #!/usr/bin/env bash
        in=$1
        out_h=$2
        out_cpp=$3
    
        echo "// This file is auto generated. Do not edit." > $out_h
        echo "#pragma once" >> $out_h
        echo "#include <string_view>" >> $out_h
        echo "extern std::string_view hardcoded_ca_certificates;" >> $out_h
    
        echo "// This file is auto generated. Do not edit." > $out_cpp
        echo "#include \"cacert.pem.h\"" >> $out_cpp
        echo "std::string_view hardcoded_ca_certificates =" >> $out_cpp
        while read line; do
            echo "    \"$line\\n\"" >> $out_cpp
        done < $in
        echo ";" >> $out_cpp
    ]=])
    
    add_custom_command(
        COMMAND
            bash ${OUINET_CA_CERTS_CONVERTER} ${OUINET_CA_CERTS_PEM} ${OUINET_CA_CERTS_H} ${OUINET_CA_CERTS_CPP}
        OUTPUT ${OUINET_CA_CERTS_H} ${OUINET_CA_CERTS_CPP}
        DEPENDS ${OUINET_CA_CERTS_PEM} ${OUINET_CA_CERTS_CONVERTER}
        VERBATIM
    )
    
    add_library(hardcoded_ca_certs STATIC ${OUINET_CA_CERTS_CPP})
    target_include_directories(hardcoded_ca_certs INTERFACE ${OUINET_CA_CERTS_DIR})
    target_compile_definitions(hardcoded_ca_certs PUBLIC -DOUINET_HAS_HARDCODED_CA_CERTS)
endif()

include_guard(GLOBAL)

function(nimblefix_generate_schema)
    cmake_parse_arguments(NF "" "TARGET;NAMESPACE;HEADER" "XML_FILES;MESSAGES" ${ARGN})
    if(NF_UNPARSED_ARGUMENTS OR NOT NF_TARGET OR NOT NF_NAMESPACE OR NOT NF_HEADER OR NOT NF_XML_FILES)
        message(FATAL_ERROR "nimblefix_generate_schema needs TARGET, NAMESPACE, HEADER, XML_FILES")
    endif()
    if(IS_ABSOLUTE "${NF_HEADER}" OR NF_HEADER MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "HEADER must be relative to the generated include directory")
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/${NF_TARGET}")
    set(output "${output_dir}/${NF_HEADER}")
    set(xml_files)
    foreach(input IN LISTS NF_XML_FILES)
        get_filename_component(input "${input}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND xml_files "${input}")
    endforeach()
    set(message_args)
    if(NF_MESSAGES)
        string(JOIN "," messages_csv ${NF_MESSAGES})
        list(APPEND message_args --messages "${messages_csv}")
    endif()
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${NIMBLEFIX_SCHEMA_GENERATOR}"
            --namespace "${NF_NAMESPACE}" --output "${output}" ${message_args} ${xml_files}
        DEPENDS ${xml_files} "${NIMBLEFIX_SCHEMA_GENERATOR}"
        COMMENT "Generating ${NF_HEADER} from XML" VERBATIM)
    add_custom_target(${NF_TARGET}_generate ALL DEPENDS "${output}")
    add_library(${NF_TARGET} INTERFACE)
    add_dependencies(${NF_TARGET} ${NF_TARGET}_generate)
    target_compile_features(${NF_TARGET} INTERFACE cxx_std_20)
    target_include_directories(${NF_TARGET} INTERFACE
        $<BUILD_INTERFACE:${output_dir}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(${NF_TARGET} INTERFACE NimbleFIX::nimblefix)
    set(${NF_TARGET}_HEADER "${output}" PARENT_SCOPE)
endfunction()

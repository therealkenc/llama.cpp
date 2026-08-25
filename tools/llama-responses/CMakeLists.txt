set(TARGET llama-responses)

set(CODEX_PROMPT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/prompts/codex-0.148.0.md")
set(CODEX_PROMPT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/codex-models-prompt.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CODEX_PROMPT_SOURCE}")
file(READ "${CODEX_PROMPT_SOURCE}" CODEX_PROMPT_TEXT)
string(FIND "${CODEX_PROMPT_TEXT}" ")codex_0148\"" CODEX_PROMPT_DELIMITER_OFFSET)
if (NOT CODEX_PROMPT_DELIMITER_OFFSET EQUAL -1)
    message(FATAL_ERROR "Codex prompt contains the generated C++ raw-string delimiter")
endif()
configure_file(codex-models-prompt.h.in "${CODEX_PROMPT_HEADER}" @ONLY)

# A static foundation intended to be linked into llama-server. There is no
# second HTTP stack or process boundary here; protocol routing can be added by
# the owning executable with ordinary C++ dependency injection.
add_library(${TARGET} STATIC
    ${CODEX_PROMPT_HEADER}
    codex-models.cpp
    codex-models.h
    hosted-tools.cpp
    hosted-tools.h
    protocol-codec.cpp
    protocol-codec.h
    response-store.cpp
    response-store.h
    response-service.cpp
    response-service.h
    response-types.cpp
    response-types.h
    server-integration.cpp
    server-integration.h
)

target_include_directories(${TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../server
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(${TARGET} PUBLIC llama-common ${CMAKE_THREAD_LIBS_INIT})
target_compile_features(${TARGET} PUBLIC cxx_std_17)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if (LLAMA_BUILD_TESTS)
    add_executable(test-llama-responses-foundation
        tests/test-codex-models.cpp
        tests/test-foundation.cpp
        tests/test-protocol.cpp
    )
    target_link_libraries(test-llama-responses-foundation PRIVATE llama-responses)
    target_compile_features(test-llama-responses-foundation PRIVATE cxx_std_17)
    add_test(NAME test-llama-responses-foundation COMMAND test-llama-responses-foundation)
endif()

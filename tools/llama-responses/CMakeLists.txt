set(TARGET llama-responses)

find_package(SQLite3 REQUIRED)

set(CODEX_PROMPT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/prompts/codex-base-instructions.md")
set(CODEX_PROMPT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/codex-models-prompt.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CODEX_PROMPT_SOURCE}")
file(READ "${CODEX_PROMPT_SOURCE}" CODEX_PROMPT_TEXT)
string(FIND "${CODEX_PROMPT_TEXT}" ")codex_prompt\"" CODEX_PROMPT_DELIMITER_OFFSET)
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
    generation.cpp
    generation.h
    hosted-tools.cpp
    hosted-tools.h
    input-lowering.cpp
    input-lowering.h
    protocol-codec.cpp
    protocol-codec.h
    response-store.cpp
    response-store.h
    response-store-internal.h
    sqlite-response-store.cpp
    sqlite-response-store.h
    response-service.cpp
    response-service.h
    response-types.cpp
    response-types.h
    server-generation-adapter.cpp
    server-generation-adapter.h
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
target_link_libraries(${TARGET} PUBLIC llama-common ${CMAKE_THREAD_LIBS_INIT} PRIVATE SQLite::SQLite3)
target_compile_features(${TARGET} PUBLIC cxx_std_17)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if (LLAMA_BUILD_TESTS)
    add_executable(test-llama-responses-foundation
        tests/test-codex-models.cpp
        tests/test-foundation.cpp
        tests/test-generation.cpp
        tests/test-protocol.cpp
    )
    target_link_libraries(test-llama-responses-foundation PRIVATE llama-responses)
    target_compile_features(test-llama-responses-foundation PRIVATE cxx_std_17)
    add_test(NAME test-llama-responses-foundation COMMAND test-llama-responses-foundation)

    add_executable(test-llama-responses-generation-adapter
        tests/test-server-generation-adapter.cpp
    )
    target_link_libraries(test-llama-responses-generation-adapter PRIVATE llama-responses)
    target_compile_features(test-llama-responses-generation-adapter PRIVATE cxx_std_17)
    add_test(NAME test-llama-responses-generation-adapter COMMAND test-llama-responses-generation-adapter)

    add_executable(test-llama-responses-input-lowering
        tests/test-input-lowering.cpp
    )
    target_link_libraries(test-llama-responses-input-lowering PRIVATE llama-responses)
    target_compile_features(test-llama-responses-input-lowering PRIVATE cxx_std_17)
    add_test(NAME test-llama-responses-input-lowering COMMAND test-llama-responses-input-lowering)

    add_executable(test-llama-responses-route-integration
        tests/test-route-integration.cpp
    )
    target_link_libraries(test-llama-responses-route-integration PRIVATE llama-responses)
    target_compile_features(test-llama-responses-route-integration PRIVATE cxx_std_17)
    add_test(NAME test-llama-responses-route-integration COMMAND test-llama-responses-route-integration)
    set_tests_properties(test-llama-responses-route-integration PROPERTIES ENVIRONMENT "LLAMA_RESPONSES_DB=:memory:")

    add_executable(test-llama-responses-sqlite-store
        tests/test-sqlite-store.cpp
    )
    target_link_libraries(test-llama-responses-sqlite-store PRIVATE llama-responses)
    target_compile_features(test-llama-responses-sqlite-store PRIVATE cxx_std_17)
    add_test(NAME test-llama-responses-sqlite-store COMMAND test-llama-responses-sqlite-store)
endif()

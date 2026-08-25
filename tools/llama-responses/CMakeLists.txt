set(TARGET llama-responses)

# A static foundation intended to be linked into llama-server. There is no
# second HTTP stack or process boundary here; protocol routing can be added by
# the owning executable with ordinary C++ dependency injection.
add_library(${TARGET} STATIC
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

target_include_directories(${TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../server
)
target_link_libraries(${TARGET} PUBLIC llama-common ${CMAKE_THREAD_LIBS_INIT})
target_compile_features(${TARGET} PUBLIC cxx_std_17)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if (LLAMA_BUILD_TESTS)
    add_executable(test-llama-responses-foundation
        tests/test-foundation.cpp
        tests/test-protocol.cpp
    )
    target_link_libraries(test-llama-responses-foundation PRIVATE llama-responses)
    target_compile_features(test-llama-responses-foundation PRIVATE cxx_std_17)
    add_test(NAME test-llama-responses-foundation COMMAND test-llama-responses-foundation)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# server-context containing the core server logic, used by llama-server and CLI

set(TARGET server-context)

add_library(${TARGET} STATIC
    server-chat.cpp
    server-chat.h
    server-task.cpp
    server-task.h
    server-queue.cpp
    server-queue.h
    server-common.cpp
    server-common.h
    server-context.cpp
    server-context.h
    server-generation.cpp
    server-generation.h
    server-generation-internal.h
    server-responses.h
    server-stream.cpp
    server-stream.h
    server-tools.cpp
    server-tools.h
    server-mcp.cpp
    server-mcp.h
    server-schema.cpp
    server-schema.h
)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC llama-common mtmd ${CMAKE_THREAD_LIBS_INIT})

if (LLAMA_BUILD_TESTS)
    add_executable(test-llama-responses-server-chat
        ../llama-responses/tests/test-server-chat.cpp
    )
    target_link_libraries(test-llama-responses-server-chat PRIVATE server-context llama-responses)
    target_compile_features(test-llama-responses-server-chat PRIVATE cxx_std_17)
    add_test(NAME test-llama-responses-server-chat COMMAND test-llama-responses-server-chat)

    add_executable(test-server-generation-sink
        tests/test-generation-sink.cpp
    )
    target_link_libraries(test-server-generation-sink PRIVATE server-context)
    target_compile_features(test-server-generation-sink PRIVATE cxx_std_17)
    add_test(NAME test-server-generation-sink COMMAND test-server-generation-sink)
endif()

# llama-server-impl: server logic, reusable by app

set(TARGET llama-server-impl)

add_library(${TARGET}
    server.cpp
    server.h
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
    server-route-extensions.h
)
set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ../mtmd ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC server-context llama-ui cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} LIBRARY)
endif()

# llama-server executable

set(TARGET llama-server)

add_executable(${TARGET} main.cpp)
install(TARGETS ${TARGET} RUNTIME)

target_link_libraries(${TARGET} PRIVATE llama-server-impl llama-responses)
target_compile_features(${TARGET} PRIVATE cxx_std_17)

cmake_minimum_required(VERSION 3.15)
project(remote_cmd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(remote_cmd_host
    src/host_main.cpp
    src/protocol.cpp
)

add_executable(remote_cmd_agent
    src/agent_main.cpp
    src/protocol.cpp
    src/process_exec.cpp
)

foreach(tgt remote_cmd_host remote_cmd_agent)
    target_include_directories(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/include)

    if (WIN32)
        target_link_libraries(${tgt} PRIVATE ws2_32)
        target_link_options(${tgt} PRIVATE -static)
    else()
        target_link_libraries(${tgt} PRIVATE pthread)
    endif()
endforeach()

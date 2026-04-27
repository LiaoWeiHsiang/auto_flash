cmake_minimum_required(VERSION 3.10)
project(simple_web)

set(CMAKE_CXX_STANDARD 17)

add_executable(server server.cpp)
target_link_libraries(server pthread)
cmake_minimum_required(VERSION 3.15)

project(auto_flash_tools
    VERSION 1.0
    LANGUAGES CXX
)
# include(${CMAKE_SOURCE_DIR}/config.cmake)

# ===== Build Options =====
option(BUILD_AUTO_FLASH "Build auto_flash tool" OFF)
option(BUILD_FLASH_EXE  "Build flash_exe tool"  OFF)
option(BUILD_HOST_SERVER  "Build flash_exe tool"  OFF)


# ===== C++ Standard =====
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===== Common Sources =====
set(COMMON_SOURCES
    auto_flash/comport/comport.cpp
    auto_flash/flash/flash.cpp
)

set(AUTO_FLASH_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/auto_flash
)

set(COMMON_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/common
)

set(UTILS_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/utils
)

set(HOST_SERVER_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/host_server
)


set(COMMON_LIBS
    kernel32
    ws2_32
    setupapi
)

# ===== auto_flash =====
if (BUILD_AUTO_FLASH)
    add_executable(auto_flash
        auto_flash/auto_flash.cpp
        utils/talker/talker.cpp
        utils/socket/socket.cpp
        utils/listener/listener.cpp
        ${COMMON_SOURCES}
    )

    target_include_directories(auto_flash PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${AUTO_FLASH_INCLUDE_DIRS}
        ${COMMON_INCLUDE_DIRS}
        ${UTILS_INCLUDE_DIRS}
    )

    target_link_libraries(auto_flash PRIVATE
        ${COMMON_LIBS}
    )

    target_link_options(auto_flash PRIVATE -static)
endif()



# ===== flash_exe =====
if (BUILD_FLASH_EXE)
    add_executable(flash_exe
        auto_flash/flash_exe.cpp
        ${COMMON_SOURCES}
    )

    target_include_directories(flash_exe PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${AUTO_FLASH_INCLUDE_DIRS}
        ${COMMON_INCLUDE_DIRS}
        ${UTILS_INCLUDE_DIRS}
    )

    target_link_libraries(flash_exe PRIVATE
        ${COMMON_LIBS}
    )

    target_link_options(flash_exe PRIVATE -static)
endif()


if(BUILD_HOST_SERVER)
    add_executable(server 
        host_server/server.cpp
        utils/talker/talker.cpp
        utils/listener/listener.cpp
        utils/socket/socket.cpp
    )

    target_include_directories(server PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${HOST_SERVER_INCLUDE_DIRS}
    )

    target_link_libraries(server pthread)
endif()
cmake_minimum_required(VERSION 3.15)

project(auto_flash_tools
    VERSION 1.0
    LANGUAGES CXX
)
include(${CMAKE_SOURCE_DIR}/config.cmake)

# ===== Build Options =====
option(BUILD_AUTO_FLASH "Build auto_flash tool" ON)
option(BUILD_FLASH_EXE  "Build flash_exe tool"  OFF)

# ===== C++ Standard =====
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===== Common Sources =====
set(COMMON_SOURCES
    comport/comport.cpp
    flash/flash.cpp
)

set(COMMON_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}
)

set(COMMON_LIBS
    kernel32
    ws2_32
    setupapi
)

# ===== auto_flash =====
if (BUILD_AUTO_FLASH)
    add_executable(auto_flash
        auto_flash.cpp
        ${COMMON_SOURCES}
    )

    target_include_directories(auto_flash PRIVATE
        ${COMMON_INCLUDE_DIRS}
    )

    target_link_libraries(auto_flash PRIVATE
        ${COMMON_LIBS}
    )

    target_link_options(auto_flash PRIVATE -static)
endif()

# ===== flash_exe =====
if (BUILD_FLASH_EXE)
    add_executable(flash_exe
        flash_exe.cpp
        ${COMMON_SOURCES}
    )

    target_include_directories(flash_exe PRIVATE
        ${COMMON_INCLUDE_DIRS}
    )

    target_link_libraries(flash_exe PRIVATE
        ${COMMON_LIBS}
    )

    target_link_options(flash_exe PRIVATE -static)
endif()

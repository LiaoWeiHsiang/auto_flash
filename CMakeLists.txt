cmake_minimum_required(VERSION 3.15)

project(auto_flash_tools
    VERSION 1.0
    LANGUAGES CXX
)
# include(${CMAKE_SOURCE_DIR}/config.cmake)

# ===== Build Options =====
option(BUILD_AUTO_FLASH "Build auto_flash tool" OFF)
option(BUILD_FLASH_EXE  "Build flash_exe tool"  OFF)
option(BUILD_HOST_SERVER  "Build host_server"  OFF)


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
    set(AUTO_FLASH_SOURCES
        auto_flash/auto_flash.cpp
        auto_flash/local_server.cpp
        utils/talker/talker.cpp
        utils/socket/socket.cpp
        utils/listener/listener.cpp
        utils/zip/unzip.cpp
        ${COMMON_SOURCES}
    )

    # 把 host_server/index.html 內嵌進 exe，讓 client 自己就能提供本機 WebUI。
    # 這個 client 的部署方式是「curl 一個 exe」，若改成執行時讀取相鄰的
    # index.html，只要有人忘記一起複製，本機 UI 就會默默失效。
    set(AF_INDEX_HTML_H ${CMAKE_BINARY_DIR}/generated/index_html.h)
    add_custom_command(
        OUTPUT ${AF_INDEX_HTML_H}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/embed_file.py
                ${CMAKE_SOURCE_DIR}/host_server/index.html
                ${AF_INDEX_HTML_H} kIndexHtml
        DEPENDS ${CMAKE_SOURCE_DIR}/host_server/index.html
                ${CMAKE_SOURCE_DIR}/tools/embed_file.py
        COMMENT "Embedding index.html into auto_flash for the local WebUI"
        VERBATIM
    )
    list(APPEND AUTO_FLASH_SOURCES ${AF_INDEX_HTML_H})

    # VERSIONINFO + manifest resource: unsigned MinGW binaries with no
    # version/company metadata are a common Defender ML heuristic false
    # positive trigger, on top of the -static link below.
    if (WIN32)
        enable_language(RC)
        list(APPEND AUTO_FLASH_SOURCES auto_flash/version.rc)
    endif()

    add_executable(auto_flash
        ${AUTO_FLASH_SOURCES}
    )

    target_include_directories(auto_flash PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${AUTO_FLASH_INCLUDE_DIRS}
        ${COMMON_INCLUDE_DIRS}
        ${UTILS_INCLUDE_DIRS}
        ${HOST_SERVER_INCLUDE_DIRS}                       # httplib.h
        ${CMAKE_SOURCE_DIR}/third_party/json/single_include
        ${CMAKE_BINARY_DIR}/generated                     # index_html.h
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


if(BUILD_HOST_SERVER OR BUILD_HOST_SERVER_WINDOWS)
    add_executable(server 
        host_server/server_multi_plat.cpp
        utils/talker/talker.cpp
        utils/listener/listener.cpp
        utils/socket/socket.cpp
    )

    target_include_directories(server PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${HOST_SERVER_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/third_party/json/single_include
    )

    # target_link_libraries(server pthread)
    # target_link_libraries(server ws2_32)
    if (WIN32)
        target_link_libraries(server ws2_32)
        target_link_options(server PRIVATE -static)
    else()
        target_link_libraries(server pthread)
    endif()
endif()
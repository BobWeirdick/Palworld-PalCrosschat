cmake_minimum_required(VERSION 3.22)
project(PalCrosschat)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(ExternalProject)

# --- MariaDB Connector/C (static, isolated ExternalProject to avoid UE4SS install overrides) ---
set(MARIADB_INSTALL_DIR "${CMAKE_BINARY_DIR}/mariadb-install")
set(MARIADB_INCLUDE_DIR "${MARIADB_INSTALL_DIR}/include/mariadb")
set(MARIADB_LIBRARY "${MARIADB_INSTALL_DIR}/lib/mariadb/mariadbclient.lib")

ExternalProject_Add(mariadb_connector_c_ep
    GIT_REPOSITORY https://github.com/mariadb-corporation/mariadb-connector-c.git
    GIT_TAG v3.3.14
    GIT_SHALLOW TRUE
    PREFIX "${CMAKE_BINARY_DIR}/mariadb-ep"
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${MARIADB_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DWITH_UNIT_TESTS=OFF
        -DWITH_DYNAMIC_PLUGINS=OFF
        -DWITH_MYSQLCOMPAT=OFF
        -DWITH_SSL=SCHANNEL
        -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF
        -DCLIENT_PLUGIN_CACHING_SHA2_PASSWORD=STATIC
        -DCLIENT_PLUGIN_MYSQL_NATIVE_PASSWORD=STATIC
        -DCLIENT_PLUGIN_SHA256_PASSWORD=STATIC
        -DCLIENT_PLUGIN_DIALOG=STATIC
        -DCLIENT_PLUGIN_CLIENT_ED25519=STATIC
        -DCLIENT_PLUGIN_PVIO_NPIPE=STATIC
        -DCLIENT_PLUGIN_PVIO_SHMEM=STATIC
        -DCLIENT_PLUGIN_AUTH_GSSAPI_CLIENT=STATIC
    BUILD_BYPRODUCTS "${MARIADB_LIBRARY}"
)

add_library(mariadbclient_imported STATIC IMPORTED GLOBAL)
set_target_properties(mariadbclient_imported PROPERTIES
    IMPORTED_LOCATION "${MARIADB_LIBRARY}"
)
add_dependencies(mariadbclient_imported mariadb_connector_c_ep)

# UE4SS lives at c:/Repos/RE-UE4SS (sibling of Palworld-Mods).
set(UE4SS_ROOT "${CMAKE_SOURCE_DIR}/../../RE-UE4SS" CACHE PATH "Path to RE-UE4SS")
if(NOT EXISTS "${UE4SS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "RE-UE4SS not found at ${UE4SS_ROOT}. Set -DUE4SS_ROOT=...")
endif()

# nlohmann/json lives at c:/Repos/json (shared with other Repos projects).
set(NLOHMANN_JSON_INCLUDE "${CMAKE_SOURCE_DIR}/../../json/include")
if(NOT EXISTS "${NLOHMANN_JSON_INCLUDE}/nlohmann/json.hpp")
    message(FATAL_ERROR "nlohmann/json not found at ${NLOHMANN_JSON_INCLUDE}")
endif()

add_subdirectory("${UE4SS_ROOT}" "${CMAKE_BINARY_DIR}/RE-UE4SS")

set(TARGET PalCrosschat)

add_library(${TARGET} SHARED
    src/PalCrosschat.cpp
    src/Config.cpp
    src/Sanitize.cpp
    src/DbWorker.cpp
    src/ChatCapture.cpp
    src/ChatInject.cpp
    src/WordFilter.cpp
    src/Webhook.cpp
)

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${MARIADB_INCLUDE_DIR}"
    "${MARIADB_INSTALL_DIR}/include"
    "${NLOHMANN_JSON_INCLUDE}"
)

target_compile_definitions(${TARGET} PRIVATE MARIADB_STATIC=1)

target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE
    mariadbclient_imported
    ws2_32
    winhttp
    secur32
    crypt32
    shlwapi
    bcrypt
    advapi32
)

add_dependencies(${TARGET} mariadb_connector_c_ep)

set(DIST_DIR "${CMAKE_SOURCE_DIR}/dist/PalCrosschat")

set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME "main"
    RUNTIME_OUTPUT_DIRECTORY "${DIST_DIR}/dlls"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DIST_DIR}/dlls"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DIST_DIR}/dlls"
    RUNTIME_OUTPUT_DIRECTORY_Game__Shipping__Win64 "${DIST_DIR}/dlls"
    LIBRARY_OUTPUT_DIRECTORY "${DIST_DIR}/dlls"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${DIST_DIR}/dlls"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${DIST_DIR}/dlls"
    LIBRARY_OUTPUT_DIRECTORY_Game__Shipping__Win64 "${DIST_DIR}/dlls"
    ARCHIVE_OUTPUT_DIRECTORY "${DIST_DIR}/dlls"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${DIST_DIR}/dlls"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${DIST_DIR}/dlls"
    ARCHIVE_OUTPUT_DIRECTORY_Game__Shipping__Win64 "${DIST_DIR}/dlls"
)

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/dlls"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/config.json.example"
        "${DIST_DIR}/config.json.example"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/dist/PalCrosschat/enabled.txt"
        "${DIST_DIR}/enabled.txt"
    COMMENT "Packaging PalCrosschat into dist/PalCrosschat"
)

# Optional: install into a live PalServer Mods tree.
if(DEFINED PALSERVER_MODS_DIR AND NOT "${PALSERVER_MODS_DIR}" STREQUAL "")
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PALSERVER_MODS_DIR}/PalCrosschat/dlls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${TARGET}>
            "${PALSERVER_MODS_DIR}/PalCrosschat/dlls/main.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/config.json.example"
            "${PALSERVER_MODS_DIR}/PalCrosschat/config.json.example"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/dist/PalCrosschat/enabled.txt"
            "${PALSERVER_MODS_DIR}/PalCrosschat/enabled.txt"
        COMMENT "Installing PalCrosschat to ${PALSERVER_MODS_DIR}"
    )
endif()

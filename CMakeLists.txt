# Example CMake command line to create project build files:
#
# cmake -B build .
# cmake -B build -DPARAM_BACKEND_SQLITE=OFF -DPARAM_REMOTE=OFF .
#
# DelegateMQ is fetched from GitHub at the release tag DMQ_GIT_TAG when CMake
# configures. To build against a local DelegateMQ checkout instead, add
# -DFETCHCONTENT_SOURCE_DIR_DELEGATEMQ=<path>. To configure again without
# network access, add -DFETCHCONTENT_FULLY_DISCONNECTED=ON.

cmake_minimum_required(VERSION 3.16)

project(DeviceParams VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Standard C file APIs (fopen etc.) are used for portability
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# device-params options
option(PARAM_REMOTE         "Build ParamService/ParamClient (remote access over DataBus)" ON)
if(EXISTS "${CMAKE_SOURCE_DIR}/third-party/sqlite/sqlite3.c")
    set(PARAM_SQLITE_DEFAULT ON)
else()
    set(PARAM_SQLITE_DEFAULT OFF)
endif()
option(PARAM_BACKEND_SQLITE "Build SqliteBackend (needs third-party/sqlite)" ${PARAM_SQLITE_DEFAULT})

# DelegateMQ build options
set(DMQ_ALLOCATOR "OFF")
set(DMQ_UTIL "ON")
set(DMQ_THREAD "DMQ_THREAD_STDLIB")
set(DMQ_TRANSPORT "DMQ_TRANSPORT_NONE")
if(PARAM_REMOTE)
    set(DMQ_DATABUS "ON")
    set(DMQ_SERIALIZE "DMQ_SERIALIZE_SERIALIZE")
else()
    set(DMQ_DATABUS "OFF")
    set(DMQ_SERIALIZE "DMQ_SERIALIZE_NONE")
endif()

# DelegateMQ release to build against (.github/workflows/embedded.yml reads it too)
set(DMQ_GIT_TAG "v2.4.1" CACHE STRING "DelegateMQ release tag (or branch) to fetch")

include(FetchContent)
FetchContent_Declare(
    delegatemq
    GIT_REPOSITORY https://github.com/DelegateMQ/DelegateMQ.git
    GIT_TAG        ${DMQ_GIT_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(delegatemq)

# Library sources
set(PARAM_SOURCES
    ${CMAKE_SOURCE_DIR}/params/Param.h
    ${CMAKE_SOURCE_DIR}/params/ParamConfig.h
    ${CMAKE_SOURCE_DIR}/params/Record.h
    ${CMAKE_SOURCE_DIR}/params/ParamStore.h
    ${CMAKE_SOURCE_DIR}/params/ParamStore.cpp
)
set(BACKEND_SOURCES
    ${CMAKE_SOURCE_DIR}/params/backends/IBackend.h
    ${CMAKE_SOURCE_DIR}/params/backends/RamBackend.h
    ${CMAKE_SOURCE_DIR}/params/backends/FileBackend.h
    ${CMAKE_SOURCE_DIR}/params/backends/FileBackend.cpp
    ${CMAKE_SOURCE_DIR}/params/backends/FlashLogBackend.h
    ${CMAKE_SOURCE_DIR}/params/backends/FlashLogBackend.cpp
)
if(PARAM_REMOTE)
    list(APPEND PARAM_SOURCES
        ${CMAKE_SOURCE_DIR}/params/ParamMessages.h
        ${CMAKE_SOURCE_DIR}/params/ParamService.h
        ${CMAKE_SOURCE_DIR}/params/ParamService.cpp
        ${CMAKE_SOURCE_DIR}/params/ParamClient.h
        ${CMAKE_SOURCE_DIR}/params/ParamClient.cpp
    )
endif()
if(PARAM_BACKEND_SQLITE)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third-party/sqlite/sqlite3.c")
        message(FATAL_ERROR "PARAM_BACKEND_SQLITE=ON needs the SQLite amalgamation in third-party/sqlite (see README there)")
    endif()
    list(APPEND BACKEND_SOURCES
        ${CMAKE_SOURCE_DIR}/params/backends/SqliteBackend.h
        ${CMAKE_SOURCE_DIR}/params/backends/SqliteBackend.cpp
        ${CMAKE_SOURCE_DIR}/third-party/sqlite/sqlite3.c
    )
endif()

# Application sources
file(GLOB SOURCES         "${CMAKE_SOURCE_DIR}/main.cpp")
file(GLOB EXAMPLE_SOURCES "${CMAKE_SOURCE_DIR}/examples/*.cpp"
                          "${CMAKE_SOURCE_DIR}/examples/*.h")
file(GLOB TEST_SOURCES    "${CMAKE_SOURCE_DIR}/unit-tests/*.cpp"
                          "${CMAKE_SOURCE_DIR}/unit-tests/*.h")

list(APPEND SOURCES ${PARAM_SOURCES} ${BACKEND_SOURCES} ${EXAMPLE_SOURCES} ${TEST_SOURCES})

# Organize files in IDE (Visual Studio Solution Explorer)
source_group("Params"         FILES ${PARAM_SOURCES})
source_group("Backends"       FILES ${BACKEND_SOURCES})
source_group("Examples"       FILES ${EXAMPLE_SOURCES})
source_group("Unit Tests"     FILES ${TEST_SOURCES})

include_directories(
    ${CMAKE_SOURCE_DIR}
)
if(PARAM_BACKEND_SQLITE)
    include_directories(${CMAKE_SOURCE_DIR}/third-party/sqlite)
    # Third-party C source: build without project warnings
    if(MSVC)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/third-party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS "/W0")
    else()
        set_source_files_properties(${CMAKE_SOURCE_DIR}/third-party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endif()

add_executable(DeviceParamsApp ${SOURCES})

# DelegateMQ's include directories, compile definitions and library
target_link_libraries(DeviceParamsApp PRIVATE DelegateMQ::DelegateMQ)

if(PARAM_REMOTE)
    target_compile_definitions(DeviceParamsApp PRIVATE PARAM_REMOTE)
endif()
if(PARAM_BACKEND_SQLITE)
    target_compile_definitions(DeviceParamsApp PRIVATE PARAM_BACKEND_SQLITE)
    target_link_libraries(DeviceParamsApp PRIVATE ${CMAKE_DL_LIBS})
endif()

find_package(Threads REQUIRED)
target_link_libraries(DeviceParamsApp PRIVATE Threads::Threads)

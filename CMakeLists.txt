cmake_minimum_required(VERSION 3.16)
project(GTAVI-Discord VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(GTAVI_CORE_SOURCES
    src/common.c
    src/config.c
    src/control.c
    src/detect.c
    src/ipc.c
    src/json.c
    src/log.c
    src/presence.c
    src/wake.c)

# A self contained exe: link the C runtime statically so no Visual C++
# redistributable is required on the target machine.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(gtavi_core STATIC ${GTAVI_CORE_SOURCES})
target_include_directories(gtavi_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(GTAVI-Discord src/main.c)
target_link_libraries(GTAVI-Discord PRIVATE gtavi_core)

# ---------------------------------------------------------------- compiler

if(MSVC)
    add_compile_options(/W4 /utf-8 /O2 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2)
    add_compile_definitions(_DEFAULT_SOURCE)
endif()

# ----------------------------------------------------------------- windows

if(WIN32)
    # GUI subsystem: double clicking the exe must not open a console window.
    # A console is attached on demand when the program is started from cmd or
    # with --console.
    set_target_properties(GTAVI-Discord PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(GTAVI-Discord PRIVATE /ENTRY:mainCRTStartup)
    else()
        target_link_options(GTAVI-Discord PRIVATE -Wl,--subsystem,windows -Wl,-e,mainCRTStartup)
    endif()
    target_link_libraries(GTAVI-Discord PRIVATE user32 advapi32 kernel32)
    target_link_libraries(gtavi_core PUBLIC kernel32)

    # Application icon: drop assets/icon.ico into the repository and the next
    # build embeds it into the exe.
    set(GTAVI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    if(EXISTS "${GTAVI_ICON}")
        enable_language(RC)
        set(GTAVI_RC "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
        file(WRITE "${GTAVI_RC}" "IDI_ICON1 ICON \"${GTAVI_ICON}\"\n")
        target_sources(GTAVI-Discord PRIVATE "${GTAVI_RC}")
    endif()

    if(MINGW)
        # Ship without external runtime DLLs.
        target_link_options(GTAVI-Discord PRIVATE -static -s)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(gtavi_core PUBLIC Threads::Threads)
    target_link_libraries(GTAVI-Discord PRIVATE Threads::Threads)
    target_link_options(GTAVI-Discord PRIVATE -s)
endif()

# ------------------------------------------------------------------- tests

if(UNIX)
    enable_testing()
    foreach(test_name ipc config presence)
        add_executable(test_${test_name} tests/test_${test_name}.c)
        target_include_directories(test_${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(test_${test_name} PRIVATE gtavi_core Threads::Threads)
        add_test(NAME ${test_name} COMMAND test_${test_name})
    endforeach()
endif()

# ----------------------------------------------------------------- install

include(GNUInstallDirs)
install(TARGETS GTAVI-Discord RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

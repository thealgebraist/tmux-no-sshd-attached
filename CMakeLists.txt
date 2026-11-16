cmake_minimum_required(VERSION 3.25)
project(tmux2 VERSION 1.0.0 LANGUAGES C CXX)

# Ensure subprojects with older cmake_minimum_required remain compatible on CMake 4+
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for dependencies")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -pedantic)
endif()

# -----------------------------------------------------------------------------
# OpenSSL 3.5+ (macOS Homebrew hint & auto-detection)
# -----------------------------------------------------------------------------
if(APPLE)
    if(NOT DEFINED OPENSSL_ROOT_DIR AND NOT DEFINED ENV{OPENSSL_ROOT_DIR})
        execute_process(
            COMMAND brew --prefix openssl@3
            OUTPUT_VARIABLE HOMEBREW_OPENSSL_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(EXISTS "${HOMEBREW_OPENSSL_PREFIX}")
            set(OPENSSL_ROOT_DIR "${HOMEBREW_OPENSSL_PREFIX}" CACHE PATH "Path to OpenSSL root directory")
        endif()
    endif()
endif()

find_package(OpenSSL 3.5 REQUIRED COMPONENTS SSL Crypto)

# -----------------------------------------------------------------------------
# System Dependencies & FetchContent Fallbacks
# -----------------------------------------------------------------------------
option(TMUX2_USE_SYSTEM_DEPS "Prefer system packages over FetchContent" ON)

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
include(FetchContent)

# 1. doctest
set(DOCTEST_FOUND FALSE)
if(TMUX2_USE_SYSTEM_DEPS)
    find_package(doctest CONFIG QUIET)
    if(TARGET doctest::doctest)
        set(DOCTEST_FOUND TRUE)
    endif()
endif()

if(NOT DOCTEST_FOUND)
    message(STATUS "Using FetchContent fallback for doctest")
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(doctest)
endif()

# 2. toml++
set(TOMLPLUSPLUS_FOUND FALSE)
if(TMUX2_USE_SYSTEM_DEPS)
    find_package(tomlplusplus CONFIG QUIET)
    if(TARGET tomlplusplus::tomlplusplus)
        set(TOMLPLUSPLUS_FOUND TRUE)
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(PKG_TOMLPLUSPLUS QUIET tomlplusplus)
        if(PKG_TOMLPLUSPLUS_FOUND)
            add_library(tomlplusplus::tomlplusplus INTERFACE IMPORTED)
            target_include_directories(tomlplusplus::tomlplusplus INTERFACE ${PKG_TOMLPLUSPLUS_INCLUDE_DIRS})
            target_link_libraries(tomlplusplus::tomlplusplus INTERFACE ${PKG_TOMLPLUSPLUS_LIBRARIES})
            set(TOMLPLUSPLUS_FOUND TRUE)
        endif()
    endif()
endif()

if(NOT TOMLPLUSPLUS_FOUND)
    message(STATUS "Using FetchContent fallback for toml++")
    FetchContent_Declare(
        tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG        v3.4.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(tomlplusplus)
endif()

# 3. zstd
set(ZSTD_FOUND FALSE)
if(TMUX2_USE_SYSTEM_DEPS)
    find_package(zstd CONFIG QUIET)
    if(TARGET zstd::libzstd_shared OR TARGET zstd::libzstd_static OR TARGET zstd::libzstd)
        set(ZSTD_FOUND TRUE)
        if(NOT TARGET zstd::libzstd)
            if(TARGET zstd::libzstd_shared)
                add_library(zstd::libzstd ALIAS zstd::libzstd_shared)
            else()
                add_library(zstd::libzstd ALIAS zstd::libzstd_static)
            endif()
        endif()
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(PKG_ZSTD QUIET libzstd)
        if(PKG_ZSTD_FOUND)
            add_library(zstd::libzstd INTERFACE IMPORTED)
            target_include_directories(zstd::libzstd INTERFACE ${PKG_ZSTD_INCLUDE_DIRS})
            target_link_libraries(zstd::libzstd INTERFACE ${PKG_ZSTD_LIBRARIES})
            set(ZSTD_FOUND TRUE)
        endif()
    endif()
endif()

if(NOT ZSTD_FOUND)
    message(STATUS "Using FetchContent fallback for zstd")
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG        v1.5.6
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  build/cmake
    )
    FetchContent_MakeAvailable(zstd)
    if(TARGET libzstd_static AND NOT TARGET zstd::libzstd)
        add_library(zstd::libzstd ALIAS libzstd_static)
    elseif(TARGET libzstd_shared AND NOT TARGET zstd::libzstd)
        add_library(zstd::libzstd ALIAS libzstd_shared)
    endif()
endif()

# 4. libvterm
set(VTERM_FOUND FALSE)
if(TMUX2_USE_SYSTEM_DEPS AND PKG_CONFIG_FOUND)
    pkg_check_modules(PKG_VTERM QUIET vterm)
    if(PKG_VTERM_FOUND)
        add_library(vterm::vterm INTERFACE IMPORTED)
        target_include_directories(vterm::vterm INTERFACE ${PKG_VTERM_INCLUDE_DIRS})
        target_link_libraries(vterm::vterm INTERFACE ${PKG_VTERM_LIBRARIES})
        set(VTERM_FOUND TRUE)
    endif()
endif()

if(NOT VTERM_FOUND)
    message(STATUS "Using FetchContent fallback for libvterm")
    FetchContent_Declare(
        vterm
        GIT_REPOSITORY https://github.com/neovim/libvterm.git
        GIT_TAG        v0.3.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(vterm)
    if(NOT vterm_POPULATED)
        FetchContent_Populate(vterm)
        find_package(Perl REQUIRED)
        execute_process(
            COMMAND ${PERL_EXECUTABLE} -CSD tbl2inc_c.pl src/encoding/DECdrawing.tbl
            WORKING_DIRECTORY ${vterm_SOURCE_DIR}
            OUTPUT_FILE ${vterm_SOURCE_DIR}/src/encoding/DECdrawing.inc
        )
        execute_process(
            COMMAND ${PERL_EXECUTABLE} -CSD tbl2inc_c.pl src/encoding/uk.tbl
            WORKING_DIRECTORY ${vterm_SOURCE_DIR}
            OUTPUT_FILE ${vterm_SOURCE_DIR}/src/encoding/uk.inc
        )
        set(VTERM_SRCS
            ${vterm_SOURCE_DIR}/src/encoding.c
            ${vterm_SOURCE_DIR}/src/keyboard.c
            ${vterm_SOURCE_DIR}/src/mouse.c
            ${vterm_SOURCE_DIR}/src/parser.c
            ${vterm_SOURCE_DIR}/src/pen.c
            ${vterm_SOURCE_DIR}/src/screen.c
            ${vterm_SOURCE_DIR}/src/state.c
            ${vterm_SOURCE_DIR}/src/unicode.c
            ${vterm_SOURCE_DIR}/src/vterm.c
        )
        add_library(vterm STATIC ${VTERM_SRCS})
        set_target_properties(vterm PROPERTIES C_STANDARD 99)
        target_include_directories(vterm PUBLIC
            $<BUILD_INTERFACE:${vterm_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${vterm_SOURCE_DIR}/src>
        )
        add_library(vterm::vterm ALIAS vterm)
        set(VTERM_FOUND TRUE)
    endif()
endif()

# Platform libraries (pty / openpty, dl)
set(PLATFORM_LIBS "")
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    list(APPEND PLATFORM_LIBS util dl)
elseif(APPLE)
    find_library(UTIL_LIB util)
    if(UTIL_LIB)
        list(APPEND PLATFORM_LIBS ${UTIL_LIB})
    endif()
endif()

# -----------------------------------------------------------------------------
# Static Core Library: tmux2_core
# -----------------------------------------------------------------------------
set(TMUX2_CORE_SOURCES
    src/reactor/reactor.cpp
    src/pki/crypto_utils.cpp
    src/pki/cert_generator.cpp
    src/pki/identity_validator.cpp
    src/pki/pki_manager.cpp
    src/config/config_parser.cpp
    src/protocol/compression.cpp
    src/protocol/messages.cpp
    src/protocol/cell.cpp
    src/pty/pty_process.cpp
    src/pty/terminfo.cpp
    src/vte/vte_wrapper.cpp
    src/vte/scrollback_buffer.cpp
    src/vte/dirty_tracker.cpp
    src/platform/platform.cpp
    src/server/window.cpp
    src/server/client_state.cpp
    src/server/session.cpp
    src/server/session_manager.cpp
    src/server/layout.cpp
    src/server/command_dsl.cpp
    src/server/server_engine.cpp
    src/server/pane.cpp
    src/client/renderer.cpp
    src/client/client_app.cpp
    src/client/session_chooser.cpp
    src/client/keybindings.cpp
    src/client/reconnect_buffer.cpp
    src/client/input_parser.cpp
    src/client/tty_raw.cpp
    src/client/copy_mode.cpp
    src/client/cli_parser.cpp
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/transport.hpp")
    list(APPEND TMUX2_CORE_SOURCES
        src/transport/tls_utils.cpp
        src/transport/happy_eyeballs.cpp
        src/transport/tls_tcp_transport.cpp
        src/transport/quic_transport.cpp
        src/transport/unix_transport.cpp
    )
endif()

add_library(tmux2_core STATIC ${TMUX2_CORE_SOURCES})

target_include_directories(tmux2_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(tmux2_core PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    zstd::libzstd
    tomlplusplus::tomlplusplus
    vterm::vterm
    Threads::Threads
    ${PLATFORM_LIBS}
)

# -----------------------------------------------------------------------------
# Unified Executable: tmux2 & tmux2d symlink
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    add_executable(tmux2 src/main.cpp)
    target_link_libraries(tmux2 PRIVATE tmux2_core)

    add_custom_command(
        TARGET tmux2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:tmux2> $<TARGET_FILE_DIR:tmux2>/tmux2d
        COMMENT "Creating tmux2d symlink -> tmux2"
    )
endif()

# -----------------------------------------------------------------------------
# Test Suites
# -----------------------------------------------------------------------------
enable_testing()

set(TEST_SOURCES
    tests/test_reactor.cpp
    tests/test_pki_config.cpp
    tests/test_protocol.cpp
    tests/test_pty_vte.cpp
    tests/test_transport.cpp
    tests/test_server.cpp
    tests/test_client.cpp
)

foreach(TSRC IN LISTS TEST_SOURCES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${TSRC}")
        get_filename_component(TNAME ${TSRC} NAME_WE)
        add_executable(${TNAME} ${TSRC})
        target_link_libraries(${TNAME} PRIVATE tmux2_core doctest::doctest)
        add_test(NAME ${TNAME} COMMAND ${TNAME})
    endif()
endforeach()

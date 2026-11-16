cmake_minimum_required(VERSION 3.25)
project(tmux2 VERSION 1.0.0 LANGUAGES C CXX)

# Ensure subprojects with older cmake_minimum_required remain compatible on CMake 4+
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for dependencies")
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall>
                        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
                        $<$<COMPILE_LANGUAGE:CXX>:-pedantic>)
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
        elseif(EXISTS "/opt/homebrew/opt/openssl@3")
            set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3" CACHE PATH "Path to OpenSSL root directory")
        elseif(EXISTS "/usr/local/opt/openssl@3")
            set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3" CACHE PATH "Path to OpenSSL root directory")
        endif()
    endif()
endif()

find_package(OpenSSL 3.5 QUIET COMPONENTS SSL Crypto)
if(NOT OpenSSL_FOUND)
    if(APPLE)
        message(FATAL_ERROR
            "OpenSSL 3.5+ (COMPONENTS SSL Crypto) was not found.\n"
            "On macOS, please install OpenSSL 3 via Homebrew:\n"
            "  brew install openssl@3\n"
            "If already installed in a custom location, specify OPENSSL_ROOT_DIR:\n"
            "  cmake -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)\n"
        )
    else()
        message(FATAL_ERROR
            "OpenSSL 3.5+ (COMPONENTS SSL Crypto) was not found.\n"
            "Please install OpenSSL 3.5+ development packages or specify OPENSSL_ROOT_DIR:\n"
            "  cmake -B build -DOPENSSL_ROOT_DIR=/path/to/openssl\n"
        )
    endif()
endif()

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
    else()
        find_path(DOCTEST_INCLUDE_DIR NAMES doctest/doctest.h)
        if(DOCTEST_INCLUDE_DIR)
            add_library(doctest::doctest INTERFACE IMPORTED)
            target_include_directories(doctest::doctest INTERFACE ${DOCTEST_INCLUDE_DIR})
            set(DOCTEST_FOUND TRUE)
        endif()
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
    if(NOT TOMLPLUSPLUS_FOUND)
        find_path(TOMLPLUSPLUS_INCLUDE_DIR NAMES toml++/toml.hpp toml.hpp)
        if(TOMLPLUSPLUS_INCLUDE_DIR)
            add_library(tomlplusplus::tomlplusplus INTERFACE IMPORTED)
            target_include_directories(tomlplusplus::tomlplusplus INTERFACE ${TOMLPLUSPLUS_INCLUDE_DIR})
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
    if(NOT ZSTD_FOUND)
        find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)
        find_library(ZSTD_LIBRARY NAMES zstd)
        if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
            add_library(zstd::libzstd INTERFACE IMPORTED)
            target_include_directories(zstd::libzstd INTERFACE ${ZSTD_INCLUDE_DIR})
            target_link_libraries(zstd::libzstd INTERFACE ${ZSTD_LIBRARY})
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
if(TMUX2_USE_SYSTEM_DEPS)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PKG_VTERM QUIET vterm)
        if(PKG_VTERM_FOUND)
            add_library(vterm::vterm INTERFACE IMPORTED)
            target_include_directories(vterm::vterm INTERFACE ${PKG_VTERM_INCLUDE_DIRS})
            target_link_libraries(vterm::vterm INTERFACE ${PKG_VTERM_LIBRARIES})
            set(VTERM_FOUND TRUE)
        endif()
    endif()
    if(NOT VTERM_FOUND)
        find_path(VTERM_INCLUDE_DIR NAMES vterm.h)
        find_library(VTERM_LIBRARY NAMES vterm)
        if(VTERM_INCLUDE_DIR AND VTERM_LIBRARY)
            add_library(vterm::vterm INTERFACE IMPORTED)
            target_include_directories(vterm::vterm INTERFACE ${VTERM_INCLUDE_DIR})
            target_link_libraries(vterm::vterm INTERFACE ${VTERM_LIBRARY})
            set(VTERM_FOUND TRUE)
        endif()
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
        find_package(Perl QUIET)
        if(PERL_FOUND)
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
        endif()
        if(NOT EXISTS "${vterm_SOURCE_DIR}/src/encoding/DECdrawing.inc")
            file(WRITE "${vterm_SOURCE_DIR}/src/encoding/DECdrawing.inc"
"static const struct StaticTableEncoding encoding_DECdrawing = {
  { .decode = &decode_table },
  {
    [0x60] = 0x25C6,
    [0x61] = 0x2592,
    [0x62] = 0x2409,
    [0x63] = 0x240C,
    [0x64] = 0x240D,
    [0x65] = 0x240A,
    [0x66] = 0x00B0,
    [0x67] = 0x00B1,
    [0x68] = 0x2424,
    [0x69] = 0x240B,
    [0x6a] = 0x2518,
    [0x6b] = 0x2510,
    [0x6c] = 0x250C,
    [0x6d] = 0x2514,
    [0x6e] = 0x253C,
    [0x6f] = 0x23BA,
    [0x70] = 0x23BB,
    [0x71] = 0x2500,
    [0x72] = 0x23BC,
    [0x73] = 0x23BD,
    [0x74] = 0x251C,
    [0x75] = 0x2524,
    [0x76] = 0x2534,
    [0x77] = 0x252C,
    [0x78] = 0x2502,
    [0x79] = 0x2A7D,
    [0x7a] = 0x2A7E,
    [0x7b] = 0x03C0,
    [0x7c] = 0x2260,
    [0x7d] = 0x00A3,
    [0x7e] = 0x00B7,
  }
};
")
        endif()
        if(NOT EXISTS "${vterm_SOURCE_DIR}/src/encoding/uk.inc")
            file(WRITE "${vterm_SOURCE_DIR}/src/encoding/uk.inc"
"static const struct StaticTableEncoding encoding_uk = {
  { .decode = &decode_table },
  {
    [0x23] = 0x00a3,
  }
};
")
        endif()

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
    src/app/pki_commands.cpp
    src/app/daemon_runner.cpp
    src/app/client_runner.cpp
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

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")

foreach(TSRC IN LISTS TEST_SOURCES)
    if(EXISTS "${TSRC}")
        get_filename_component(TNAME ${TSRC} NAME_WE)
        add_executable(${TNAME} ${TSRC})
        target_link_libraries(${TNAME} PRIVATE tmux2_core doctest::doctest)
        target_compile_definitions(${TNAME} PRIVATE TMUX2_BIN_PATH="$<TARGET_FILE:tmux2>")
        add_test(NAME ${TNAME} COMMAND ${TNAME})
    endif()
endforeach()

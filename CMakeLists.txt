cmake_minimum_required(VERSION 3.16...4.0)
project(StelliferumAuditor VERSION 2.0.0 LANGUAGES C CXX)

# Suppress deprecation warnings from dependencies (raylib cmake_minimum_required)
cmake_policy(SET CMP0169 OLD)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Platform detection
if(WIN32)
    set(PLATFORM_NAME "Windows")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    set(PLATFORM_NAME "macOS")
elseif(UNIX)
    set(PLATFORM_NAME "Linux")
endif()

message(STATUS "Building for ${PLATFORM_NAME}")

# ==============================================================================
# DEPENDENCIES
# ==============================================================================

option(USE_SYSTEM_RAYLIB "Use system-installed raylib" OFF)

if(USE_SYSTEM_RAYLIB)
    find_package(raylib REQUIRED)
else()
    include(FetchContent)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.5
        GIT_SHALLOW TRUE
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
endif()

# Dear ImGui (immediate mode GUI library)
# We use FETCHCONTENT_UPDATES_DISCONNECTED to avoid re-downloading on every configure
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(imgui)

# rlImGui (raylib backend for Dear ImGui)
FetchContent_Declare(
    rlimgui
    GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
    GIT_TAG main
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(rlimgui)

# ImGui source files (no CMakeLists in imgui repo)
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
)

# rlImGui source
set(RLIMGUI_SOURCES
    ${rlimgui_SOURCE_DIR}/rlImGui.cpp
)

# ==============================================================================
# libcurl (native FTP/SFTP) + libssh2
# ==============================================================================
# Prefer system libcurl when present; otherwise FetchContent curl + libssh2.
# STELLI_USE_LIBCURL is defined when the native backend is linked in.
#
# Cross-platform transfer (linux-no-winscp / roadmap #7):
#   - Linux/macOS: native libcurl is the REQUIRED transfer path.
#   - WinSCP.exe / resources.rc are NEVER required off Windows (optional
#     Windows-only embed when libs/winscp/WinSCP.exe exists).
#   - dependency_manager soft-skips extract/PE patch on non-Windows.

option(STELLI_USE_LIBCURL "Enable native FTP/SFTP backend via libcurl" ON)
option(STELLI_FETCH_LIBCURL "Fetch libcurl/libssh2 via FetchContent when not found" ON)

set(STELLI_LIBCURL_LINKED OFF)

if(STELLI_USE_LIBCURL)
    # Avoid pulling optional network deps that break portable Windows builds.
    set(_stelli_prev_build_shared_libs "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

    set(_stelli_have_system_curl OFF)
    if(NOT STELLI_FETCH_LIBCURL)
        find_package(CURL QUIET)
        if(CURL_FOUND)
            set(_stelli_have_system_curl ON)
        endif()
    else()
        # Still allow a preinstalled curl to short-circuit the fetch when usable.
        find_package(CURL QUIET)
        if(CURL_FOUND AND TARGET CURL::libcurl)
            # Prefer system only when SFTP feature is not required at configure time;
            # FetchContent path guarantees FTP + SFTP (libssh2) for portable builds.
            # Force FetchContent for consistent FTP+SFTP unless STELLI_FETCH_LIBCURL=OFF.
            set(_stelli_have_system_curl OFF)
        endif()
    endif()

    if(_stelli_have_system_curl)
        message(STATUS "Using system libcurl (CURL::libcurl)")
        set(STELLI_LIBCURL_LINKED ON)
    elseif(STELLI_FETCH_LIBCURL)
        message(STATUS "Fetching libssh2 + libcurl (FTP + SFTP) via FetchContent...")

        # ---- libssh2 (SFTP provider for curl) ----
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
        set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
        if(WIN32)
            # Windows CNG — no OpenSSL required for libssh2.
            set(CRYPTO_BACKEND "WinCNG" CACHE STRING "libssh2 crypto backend" FORCE)
        endif()

        FetchContent_Declare(
            libssh2
            GIT_REPOSITORY https://github.com/libssh2/libssh2.git
            GIT_TAG        libssh2-1.11.1
            GIT_SHALLOW    TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(libssh2)

        # curl's CMake uses FindLibssh2.cmake (module mode): LIBSSH2_FOUND +
        # LIBSSH2_INCLUDE_DIR / LIBSSH2_LIBRARY. Pre-seed those so find_* skips
        # the filesystem search and SFTP is enabled against our FetchContent tree.
        set(_stelli_libssh2_target "")
        if(TARGET libssh2::libssh2_static)
            set(_stelli_libssh2_target libssh2::libssh2_static)
        elseif(TARGET libssh2_static)
            set(_stelli_libssh2_target libssh2_static)
        elseif(TARGET Libssh2::libssh2)
            set(_stelli_libssh2_target Libssh2::libssh2)
        elseif(TARGET libssh2::libssh2)
            set(_stelli_libssh2_target libssh2::libssh2)
        elseif(TARGET libssh2)
            set(_stelli_libssh2_target libssh2)
        endif()

        if(NOT _stelli_libssh2_target)
            message(FATAL_ERROR "libssh2 FetchContent did not produce a usable CMake target")
        endif()

        set(LIBSSH2_INCLUDE_DIR "${libssh2_SOURCE_DIR}/include" CACHE PATH "libssh2 include dir" FORCE)
        set(LIBSSH2_LIBRARY "${_stelli_libssh2_target}" CACHE FILEPATH "libssh2 library target" FORCE)
        set(LIBSSH2_INCLUDE_DIRS "${libssh2_SOURCE_DIR}/include;${libssh2_BINARY_DIR}/src")
        set(LIBSSH2_LIBRARIES "${_stelli_libssh2_target}")
        set(LIBSSH2_FOUND TRUE)
        # Also export package-name case for CONFIG-style consumers.
        set(Libssh2_FOUND TRUE)

        message(STATUS "libssh2 target for curl: ${_stelli_libssh2_target}")

        # ---- curl (FTP + SFTP via libssh2) ----
        set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
        set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
        set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
        set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
        set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
        set(CURL_USE_LIBSSH2 ON CACHE BOOL "" FORCE)
        set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
        set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
        set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_FTP OFF CACHE BOOL "" FORCE)
        set(HTTP_ONLY OFF CACHE BOOL "" FORCE)
        set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
        set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
        set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
        set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
        set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
        set(CURL_USE_LIBIDN2 OFF CACHE BOOL "" FORCE)

        if(WIN32)
            set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
            set(CURL_WINDOWS_SSPI ON CACHE BOOL "" FORCE)
            set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        else()
            # Prefer OpenSSL on Unix when available; curl will probe.
            set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
        endif()

        FetchContent_Declare(
            curl
            GIT_REPOSITORY https://github.com/curl/curl.git
            GIT_TAG        curl-8_11_1
            GIT_SHALLOW    TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(curl)

        if(TARGET CURL::libcurl)
            set(STELLI_LIBCURL_LINKED ON)
            # Remember libssh2 so the app can link it if curl did not propagate the target.
            set(STELLI_LIBSSH2_TARGET "${_stelli_libssh2_target}" CACHE INTERNAL "libssh2 CMake target for SFTP")
            message(STATUS "libcurl (FetchContent) ready — FTP + SFTP (libssh2 via ${_stelli_libssh2_target})")
        else()
            message(WARNING "FetchContent curl did not produce CURL::libcurl target")
        endif()
    else()
        message(WARNING "STELLI_USE_LIBCURL=ON but libcurl not found and STELLI_FETCH_LIBCURL=OFF")
    endif()

    # Restore prior shared-libs preference for any later targets.
    if(DEFINED _stelli_prev_build_shared_libs)
        set(BUILD_SHARED_LIBS "${_stelli_prev_build_shared_libs}" CACHE BOOL "Build shared libraries" FORCE)
    endif()
endif()

# ==============================================================================
# SOURCES
# ==============================================================================

set(SOURCES
    src/main.c
    src/util.c
    src/parser.c
    src/auditor.c
    src/writer.c
    src/ui.cpp
    src/loot_manager.c
    src/loot_policy.c
    src/container_policy.c
    src/store_generator.c
    src/ftp_manager.c
    src/ftp_native.c
    src/gap_filler.c
    src/dependency_manager.c
    src/file_index.c
    src/stitcher.c
    src/swarm.c
    src/file_sorter.c
    src/trader_gap_filler.c
    src/web_lookup.c
    src/classname_map.c
    src/integrity.c
    src/sfl_generator.c
    src/upload_daemon.c
    src/cli_commands.c
)

set(HEADERS
    include/auditor.h
    include/loot_manager.h
    include/loot_policy.h
    include/container_policy.h
    include/resource_ids.h
    include/web_lookup.h
    include/cli_commands.h
)

# Win32 resources (optional Windows-only embed of WinSCP.exe as RCDATA fallback).
# Linux/macOS: never touch resources.rc; configure must succeed without WinSCP.exe.
set(WIN32_RESOURCES "")
if(WIN32)
    if(EXISTS "${CMAKE_SOURCE_DIR}/libs/winscp/WinSCP.exe")
        set(WIN32_RESOURCES src/resources.rc)
        message(STATUS "Embedding WinSCP.exe as Win32 resource (optional portable fallback).")
    else()
        message(STATUS "libs/winscp/WinSCP.exe not found — WinSCP will NOT be embedded (OK; native preferred).")
    endif()
else()
    message(STATUS "Non-Windows: skipping WinSCP resources.rc (native libcurl is required transfer path).")
endif()

# ==============================================================================
# EXECUTABLE
# ==============================================================================

add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${IMGUI_SOURCES} ${RLIMGUI_SOURCES} ${HEADERS} ${WIN32_RESOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${rlimgui_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE raylib)

if(STELLI_LIBCURL_LINKED)
    target_compile_definitions(${PROJECT_NAME} PRIVATE STELLI_USE_LIBCURL=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
    if(STELLI_LIBSSH2_TARGET)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${STELLI_LIBSSH2_TARGET})
    endif()
    message(STATUS "Native FTP/SFTP: STELLI_USE_LIBCURL enabled (libcurl linked)")
else()
    if(WIN32)
        message(STATUS "Native FTP/SFTP: STELLI_USE_LIBCURL not enabled (optional WinSCP fallback may apply on Windows)")
    else()
        # Off Windows there is no WinSCP fallback — native is required for transfers.
        if(STELLI_USE_LIBCURL)
            message(FATAL_ERROR
                "Native libcurl is required for FTP/SFTP on ${PLATFORM_NAME} "
                "(WinSCP is Windows-only). Enable STELLI_FETCH_LIBCURL=ON or install "
                "libcurl with FTP+SFTP (libssh2) and reconfigure. "
                "Install OpenSSL dev headers if FetchContent curl fails TLS probe "
                "(e.g. apt install libssl-dev / dnf install openssl-devel).")
        else()
            message(WARNING
                "STELLI_USE_LIBCURL=OFF on ${PLATFORM_NAME}: FTP/SFTP transfer is unavailable "
                "(no WinSCP on non-Windows). Reconfigure with -DSTELLI_USE_LIBCURL=ON.")
        endif()
    endif()
endif()

if(WIN32)
    # WIN32 subsystem suppresses the console window.
    # /ENTRY:mainCRTStartup lets us keep standard main() instead of WinMain.
    target_link_options(${PROJECT_NAME} PRIVATE /ENTRY:mainCRTStartup)
    target_link_libraries(${PROJECT_NAME} PRIVATE 
        winmm 
        comdlg32 
        ole32 
        shell32 
        wininet 
        urlmon
    )
else()
    # Unix: raylib/curl may need pthread; ensure portable link.
    find_package(Threads QUIET)
    if(Threads_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(${PROJECT_NAME} PRIVATE m dl)
    endif()
endif()

# ==============================================================================
# INSTALLATION & CONFIG
# ==============================================================================

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/ DESTINATION share/${PROJECT_NAME}/config)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/config $<TARGET_FILE_DIR:${PROJECT_NAME}>/config
    COMMENT "Copying config files to exe directory..."
)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${PROJECT_NAME}>/output
    COMMENT "Creating output directory..."
)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${PROJECT_NAME}>/backups
    COMMENT "Creating backups directory..."
)

# ==============================================================================
# COPY DEPENDENCIES (Standalone Build)
# ==============================================================================
# Windows only: optional WinSCP may be embedded as RCDATA and extracted beside
# the executable at runtime (dependency_manager) for fallback transfers.
# Linux/macOS: no WinSCP copy/extract/PE patch — native libcurl only.

# ==============================================================================
# SUMMARY
# ==============================================================================
message(STATUS "╔══════════════════════════════════════════════════════════════╗")
message(STATUS "║           STELLIFERUM LOOT AUDITOR - Build Config            ║")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")
message(STATUS "  Platform                  : ${PLATFORM_NAME}")
message(STATUS "  STELLI_USE_LIBCURL option : ${STELLI_USE_LIBCURL}")
message(STATUS "  libcurl linked            : ${STELLI_LIBCURL_LINKED}")
if(WIN32)
    if(WIN32_RESOURCES)
        message(STATUS "  WinSCP resources.rc       : embedded (optional Windows fallback)")
    else()
        message(STATUS "  WinSCP resources.rc       : not embedded (libs/winscp absent; OK)")
    endif()
    message(STATUS "  Transfer backend          : native preferred; WinSCP optional fallback")
else()
    message(STATUS "  WinSCP resources.rc       : not used on ${PLATFORM_NAME}")
    message(STATUS "  Transfer backend          : native libcurl REQUIRED (no WinSCP)")
endif()

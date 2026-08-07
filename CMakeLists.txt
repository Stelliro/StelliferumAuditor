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
    src/store_generator.c
    src/ftp_manager.c
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
)

set(HEADERS
    include/auditor.h
    include/loot_manager.h
    include/loot_policy.h
    include/resource_ids.h
    include/web_lookup.h
)

# Win32 resources (embeds WinSCP.exe as RCDATA)
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/libs/winscp/WinSCP.exe")
    set(WIN32_RESOURCES src/resources.rc)
    message(STATUS "Embedding WinSCP.exe as Win32 resource (portable bundling).")
else()
    set(WIN32_RESOURCES "")
    if(WIN32)
        message(STATUS "libs/winscp/WinSCP.exe not found — WinSCP will NOT be embedded.")
    endif()
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
# WinSCP is now embedded as a Win32 resource — no file copying needed.
# The executable self-extracts WinSCP.exe to .TEMP/ at runtime.

# ==============================================================================
# SUMMARY
# ==============================================================================
message(STATUS "╔══════════════════════════════════════════════════════════════╗")
message(STATUS "║           STELLIFERUM LOOT AUDITOR - Build Config            ║")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")

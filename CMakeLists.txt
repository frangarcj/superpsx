cmake_minimum_required(VERSION 3.13)

# Set the toolchain file before project()
if(NOT CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{PS2DEV})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{PS2DEV}/share/ps2dev.cmake" CACHE FILEPATH "Toolchain file")
    else()
        message(FATAL_ERROR "PS2DEV environment variable is not set. Please set it to your PS2DEV installation path.")
    endif()
endif()

project(superpsx VERSION 1.0.0 LANGUAGES C)

# Check if PS2SDK is set (should be done by toolchain file)
if(NOT DEFINED PS2SDK)
    message(FATAL_ERROR "PS2SDK is not defined. Make sure the toolchain file is loaded correctly.")
endif()

# ============================================================================
# Options
# ============================================================================
option(ENABLE_VRAM_DUMP "Enable VRAM dumping (reduces performance)" OFF)
option(ENABLE_HOST_LOG "Enable host logging" ON)
option(ENABLE_DEBUG_LOG "Enable debug logging" ON)
option(ENABLE_STUCK_DETECTION "Enable stuck detection" ON)
option(ENABLE_PROFILING "Build with gprof instrumentation (libprofglue)" OFF)
option(ENABLE_LTO "Enable Link-Time Optimization" OFF)
option(ENABLE_DYNAREC_STATS "Enable dynarec execution statistics" OFF)
option(ENABLE_SUBSYSTEM_PROFILER "Enable per-subsystem wall-clock profiler" ON)
option(HEADLESS "Build without GPU/video output (no-op GPU stubs)" OFF)

# ============================================================================
# Source files
# ============================================================================
if(HEADLESS)
    set(GPU_SOURCES src/gpu_stub.c)
else()
    set(GPU_SOURCES
        src/gpu_core.c
        src/gpu_gif.c
        src/gpu_vram.c
        src/gpu_texture.c
        src/gpu_primitives.c
        src/gpu_commands.c
        src/gpu_dma.c
    )
endif()

set(SUPERPSX_SOURCES
    src/main.c
    src/config.c
    src/memory.c
    src/cpu.c
    src/hardware.c
    src/dma.c
    src/sio.c
    src/timers.c
    ${GPU_SOURCES}
    src/dynarec_emit.c
    src/dynarec_cache.c
    src/dynarec_memory.c
    src/dynarec_compile.c
    src/dynarec_insn.c
    src/dynarec_run.c
    src/gte.c
    src/cdrom.c
    src/loader.c
    src/joystick.c
    src/spu.c
    src/scheduler.c
    src/iso_image.c
    src/iso_fs.c
    src/profiler.c
    src/tlb_handler.c
)

# ============================================================================
# Executable target
# ============================================================================
add_executable(${PROJECT_NAME}.elf ${SUPERPSX_SOURCES})

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# ============================================================================
# Compile definitions based on options
# ============================================================================
if(ENABLE_VRAM_DUMP)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE ENABLE_VRAM_DUMP)
endif()

if(ENABLE_HOST_LOG)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE ENABLE_HOST_LOG)
endif()

if(ENABLE_DEBUG_LOG)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE ENABLE_DEBUG_LOG)
endif()

if(ENABLE_STUCK_DETECTION)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE ENABLE_STUCK_DETECTION)
endif()

if(ENABLE_DYNAREC_STATS)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE ENABLE_DYNAREC_STATS)
endif()

if(ENABLE_SUBSYSTEM_PROFILER)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE ENABLE_SUBSYSTEM_PROFILER)
endif()

if(HEADLESS)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE HEADLESS)
endif()

# ============================================================================
# Compiler flags
# ============================================================================
target_compile_options(${PROJECT_NAME}.elf PRIVATE
    -O2 -G0 -Wall
)

if(ENABLE_PROFILING)
    target_compile_options(${PROJECT_NAME}.elf PRIVATE -pg -g)
    target_link_options(${PROJECT_NAME}.elf PRIVATE -pg -g)
endif()

if(ENABLE_LTO)
    target_compile_options(${PROJECT_NAME}.elf PRIVATE -flto)
    target_link_options(${PROJECT_NAME}.elf PRIVATE -flto)
endif()

# ============================================================================
# Link libraries
# ============================================================================

target_link_libraries(${PROJECT_NAME}.elf PRIVATE
    -lpatches
    -lps2_drivers
    -ldebug
    -lgraph
    -ldma
    -ldraw
    -lmath3d
)

# ============================================================================
# Copy tests/ and isos/ into the build directory (symlinks for speed)
# ============================================================================
foreach(_data_dir bios tests isos tools)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_data_dir}")
        if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/${_data_dir}")
            file(CREATE_LINK
                "${CMAKE_SOURCE_DIR}/${_data_dir}"
                "${CMAKE_CURRENT_BINARY_DIR}/${_data_dir}"
                SYMBOLIC
            )
            message(STATUS "Linked ${_data_dir}/ into build directory")
        endif()
    endif()
endforeach()

# Copy superpsx.ini into the build directory if it exists
if(EXISTS "${CMAKE_SOURCE_DIR}/superpsx.ini")
    if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/superpsx.ini")
        file(CREATE_LINK
            "${CMAKE_SOURCE_DIR}/superpsx.ini"
            "${CMAKE_CURRENT_BINARY_DIR}/superpsx.ini"
            SYMBOLIC
        )
        message(STATUS "Linked superpsx.ini into build directory")
    endif()
endif()

# ============================================================================
# PCSX2 runner settings
# ============================================================================
if(CMAKE_HOST_APPLE)
    set(PCSX2_DEFAULT_PATH "/Applications/PCSX2.app/Contents/MacOS/PCSX2")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(PCSX2_DEFAULT_PATH "pcsx2")
else()
    set(PCSX2_DEFAULT_PATH "pcsx2")
endif()
set(PCSX2_PATH "${PCSX2_DEFAULT_PATH}" CACHE FILEPATH "Path to PCSX2 executable")
option(ENABLE_UNLIMITED_SPEED "Enable unlimited speed in PCSX2" ON)

set(PCSX2_RUN_FLAGS -batch -earlyconsolelog)
if(ENABLE_UNLIMITED_SPEED)
    list(APPEND PCSX2_RUN_FLAGS -unlimited)
endif()

# Build the base PCSX2 command as a semicolon-separated list for run_pcsx2.cmake
set(PCSX2_CMD "${PCSX2_PATH}")
foreach(_f ${PCSX2_RUN_FLAGS})
    list(APPEND PCSX2_CMD "${_f}")
endforeach()
list(APPEND PCSX2_CMD -elf "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.elf")

# Escape semicolons so the list survives -D passing
string(REPLACE ";" "\\;" PCSX2_CMD_ESC "${PCSX2_CMD}")

# ============================================================================
# Generic "run" target — pass GAMEARGS at make-time:
#   make run GAMEARGS=tests/gpu/triangle/triangle.exe
# ============================================================================
add_custom_target(run
    COMMAND ${CMAKE_COMMAND}
        "-DPCSX2_CMD=${PCSX2_CMD_ESC}"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_pcsx2.cmake"
    DEPENDS ${PROJECT_NAME}.elf
    COMMENT "Running ${PROJECT_NAME}.elf in PCSX2"
    VERBATIM
)

# ============================================================================
# Print configuration summary
# ============================================================================
message(STATUS "")
message(STATUS "superpsx configuration:")
message(STATUS "  Version:              ${PROJECT_VERSION}")
message(STATUS "  ENABLE_VRAM_DUMP:     ${ENABLE_VRAM_DUMP}")
message(STATUS "  ENABLE_HOST_LOG:      ${ENABLE_HOST_LOG}")
message(STATUS "  ENABLE_DEBUG_LOG:     ${ENABLE_DEBUG_LOG}")
message(STATUS "  ENABLE_STUCK_DETECTION: ${ENABLE_STUCK_DETECTION}")
message(STATUS "  ENABLE_PROFILING:      ${ENABLE_PROFILING}")
message(STATUS "  ENABLE_LTO:            ${ENABLE_LTO}")
message(STATUS "  ENABLE_UNLIMITED_SPEED: ${ENABLE_UNLIMITED_SPEED}")
message(STATUS "  ENABLE_DYNAREC_STATS:  ${ENABLE_DYNAREC_STATS}")
message(STATUS "  ENABLE_SUBSYSTEM_PROFILER: ${ENABLE_SUBSYSTEM_PROFILER}")
message(STATUS "  HEADLESS:              ${HEADLESS}")
message(STATUS "")
message(STATUS "  Usage:")
message(STATUS "    cmake -B build && cmake --build build")
message(STATUS "    make -C build run GAMEARGS=tests/gpu/triangle/triangle.exe")
message(STATUS "")

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
option(ENABLE_PSX_TLB "Enable TLB fast-path for PSX RAM (experimental)" OFF)
option(ENABLE_TEX_DEBUG "Enable texture debug overlay (colored bounding boxes)" OFF)
option(ENABLE_VU0_MICRO "Use VU0 micro mode for GTE acceleration" ON)
option(ENABLE_MDEC_IPU "Use PS2 IPU hardware for MDEC IDCT+YUV→RGB" OFF)
option(HEADLESS "Build without GPU/video output (no-op GPU stubs)" OFF)

# ============================================================================
# Source files
# ============================================================================
if(HEADLESS)
    set(GPU_SOURCES src/gpu_stub.c)
else()
    set(GPU_SOURCES
        src/platform/ps2/gpu_ps2_core.c
        src/platform/ps2/gpu_ps2_gif.c
        src/platform/ps2/gpu_ps2_vram.c
        src/platform/ps2/gpu_ps2_texture.c
        src/platform/ps2/gpu_ps2_primitives.c
        src/gpu_commands.c
        src/platform/ps2/gpu_ps2_dma.c
        src/platform/ps2/gpu_ps2_backend.c
        src/platform/ps2/osd_ps2.c
    )
endif()

set(SUPERPSX_SOURCES
    src/platform/ps2/main_ps2.c
    src/init.c
    src/config.c
    src/memory.c
    src/cpu.c
    src/hardware.c
    src/dma.c
    src/mdec.c
    src/mdec_ipu.c
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
    src/platform/ps2/joystick_ps2.c
    src/spu.c
    src/scheduler.c
    src/iso_image.c
    src/iso_fs.c
    src/profiler.c
    src/platform/ps2/tlb_handler_ps2.c
    src/platform/ps2/vu0_micro_ps2.c
    src/platform/ps2/platform_ps2.c
    src/platform/ps2/audio_ps2_backend.c
)

# ============================================================================
# VU0 Micro Program Assembly Pipeline
# .vsm → dvp-as → .o → ee-objcopy (.vutext→.vudata) → link directly into ELF
# No bin2c/extract needed — use extern symbols from .vsm global labels.
# ============================================================================
find_program(DVP_AS dvp-as PATHS "$ENV{PS2DEV}/dvp/bin" REQUIRED)
find_program(EE_OBJCOPY mips64r5900el-ps2-elf-objcopy PATHS "$ENV{PS2DEV}/ee/bin" REQUIRED)

set(VU0_SRC_DIR "${CMAKE_SOURCE_DIR}/src/platform/ps2/vu0")
set(VU0_GEN_DIR "${CMAKE_BINARY_DIR}/generated/vu0")
file(MAKE_DIRECTORY ${VU0_GEN_DIR})

set(VU0_PROGRAMS mvmva_full mvmva_core)
set(VU0_OBJECTS)

foreach(prog ${VU0_PROGRAMS})
    set(_vsm "${VU0_SRC_DIR}/${prog}.vsm")
    set(_dvp_obj "${VU0_GEN_DIR}/${prog}_dvp.o")
    set(_ee_obj "${VU0_GEN_DIR}/${prog}.o")

    add_custom_command(OUTPUT ${_dvp_obj}
        COMMAND ${DVP_AS} ${_vsm} -o ${_dvp_obj}
        DEPENDS ${_vsm}
        COMMENT "Assembling VU0: ${prog}.vsm"
    )
    add_custom_command(OUTPUT ${_ee_obj}
        COMMAND ${EE_OBJCOPY} --rename-section .vutext=.vudata ${_dvp_obj} ${_ee_obj}
        DEPENDS ${_dvp_obj}
        COMMENT "Converting VU0: ${prog}.o"
    )
    list(APPEND VU0_OBJECTS ${_ee_obj})
endforeach()

add_custom_target(vu0_programs DEPENDS ${VU0_OBJECTS})

# ============================================================================
# Executable target
# ============================================================================
add_executable(${PROJECT_NAME}.elf ${SUPERPSX_SOURCES})

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/platform/ps2
)

# Always define PLATFORM_PS2 for PS2 builds
target_compile_definitions(${PROJECT_NAME}.elf PRIVATE PLATFORM_PS2)

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

if(ENABLE_PSX_TLB)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE ENABLE_PSX_TLB)
endif()

if(ENABLE_TEX_DEBUG)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE TEX_DEBUG_OVERLAY)
endif()

if(ENABLE_VU0_MICRO)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE ENABLE_VU0_MICRO)
    add_dependencies(${PROJECT_NAME}.elf vu0_programs)
    target_link_libraries(${PROJECT_NAME}.elf PRIVATE ${VU0_OBJECTS})
endif()

if(ENABLE_MDEC_IPU)
    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE ENABLE_MDEC_IPU)
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
# JIT Playground — separate ELF for testing the dynarec in isolation
# ============================================================================
set(PLAYGROUND_SOURCES
    tests/jit/playground_main.c
    tests/jit/playground_tests.c
    tests/jit/test_alu.c
    tests/jit/test_memory.c
    tests/jit/test_branch.c
    tests/jit/test_block.c
    tests/jit/test_dirty.c
    tests/jit/test_gte.c
    tests/jit/test_expansion.c
    src/cpu.c
    src/memory.c
    src/scheduler.c
    src/gte.c
    src/dynarec_emit.c
    src/dynarec_cache.c
    src/dynarec_memory.c
    src/dynarec_compile.c
    src/dynarec_insn.c
    src/dynarec_run.c
    src/platform/ps2/tlb_handler_ps2.c
    src/platform/ps2/vu0_micro_ps2.c
    src/platform/ps2/platform_ps2.c
    tests/jit/test_vu0_micro.c
    tests/jit/test_gte_compare.c
)

add_executable(jit_playground.elf EXCLUDE_FROM_ALL ${PLAYGROUND_SOURCES})

target_include_directories(jit_playground.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/platform/ps2
    ${CMAKE_SOURCE_DIR}/tests/jit
)

target_compile_options(jit_playground.elf PRIVATE -O2 -G0 -Wall)
target_compile_definitions(jit_playground.elf PRIVATE JIT_PLAYGROUND ENABLE_VU0_MICRO PLATFORM_PS2)
add_dependencies(jit_playground.elf vu0_programs)

target_link_libraries(jit_playground.elf PRIVATE
    ${VU0_OBJECTS}
    -lpatches
    -lps2_drivers
)

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

# Build PCSX2 command for running the playground ELF
set(PCSX2_PG_CMD "${PCSX2_PATH}")
foreach(_f ${PCSX2_RUN_FLAGS})
    list(APPEND PCSX2_PG_CMD "${_f}")
endforeach()
list(APPEND PCSX2_PG_CMD -elf "${CMAKE_CURRENT_BINARY_DIR}/jit_playground.elf")
string(REPLACE ";" "\\;" PCSX2_PG_CMD_ESC "${PCSX2_PG_CMD}")

add_custom_target(run-playground
    COMMAND ${CMAKE_COMMAND}
        "-DPCSX2_CMD=${PCSX2_PG_CMD_ESC}"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_pcsx2.cmake"
    DEPENDS jit_playground.elf
    COMMENT "Running JIT Playground in PCSX2"
    VERBATIM
)

# ============================================================================
# GPU Playground — measure expansion of GPU commands
# ============================================================================
set(GPU_PLAYGROUND_SOURCES
    tests/playground_gpu/playground_main.c
    tests/playground_gpu/test_expansion_gpu.c
    tests/playground_gpu/test_expansion_gp1.c
    tests/playground_gpu/test_clut.c
    tests/playground_gpu/test_state.c
    tests/playground_gpu/test_vram.c
    tests/playground_gpu/test_dma_block.c
    tests/playground_gpu/test_deferred.c
    tests/playground_gpu/test_texcache.c
    tests/playground_gpu/mocks.c
    src/gpu_commands.c
    src/platform/ps2/gpu_ps2_primitives.c
    src/platform/ps2/gpu_ps2_texture.c
)

add_executable(gpu_playground.elf EXCLUDE_FROM_ALL ${GPU_PLAYGROUND_SOURCES})

target_include_directories(gpu_playground.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/platform/ps2
    ${CMAKE_SOURCE_DIR}/tests/gpu
)

target_compile_options(gpu_playground.elf PRIVATE -O2 -G0 -Wall)
target_compile_definitions(gpu_playground.elf PRIVATE GPU_PLAYGROUND PLATFORM_PS2)

target_link_libraries(gpu_playground.elf PRIVATE
    -lpatches
    -lps2_drivers
)

# Build PCSX2 command for running the GPU playground ELF
set(PCSX2_GPU_PG_CMD "${PCSX2_PATH}")
foreach(_f ${PCSX2_RUN_FLAGS})
    list(APPEND PCSX2_GPU_PG_CMD "${_f}")
endforeach()
list(APPEND PCSX2_GPU_PG_CMD -elf "${CMAKE_CURRENT_BINARY_DIR}/gpu_playground.elf")
string(REPLACE ";" "\\;" PCSX2_GPU_PG_CMD_ESC "${PCSX2_GPU_PG_CMD}")

add_custom_target(run-gpu-playground
    COMMAND ${CMAKE_COMMAND}
        "-DPCSX2_CMD=${PCSX2_GPU_PG_CMD_ESC}"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_pcsx2.cmake"
    DEPENDS gpu_playground.elf
    COMMENT "Running GPU Playground in PCSX2"
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

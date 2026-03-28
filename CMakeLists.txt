cmake_minimum_required(VERSION 3.13)

# ============================================================================
# Toolchain detection — PS2 or PSP
# ============================================================================
# PSP: cmake -S . -B build-psp -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake
# PS2: cmake -S . -B build      (auto-detects PS2DEV)
if(NOT CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{PS2DEV})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{PS2DEV}/share/ps2dev.cmake" CACHE FILEPATH "Toolchain file")
    else()
        message(FATAL_ERROR "Neither CMAKE_TOOLCHAIN_FILE nor PS2DEV is set.")
    endif()
endif()

project(superpsx VERSION 1.0.0 LANGUAGES C)

# Detect platform from toolchain
if(PLATFORM_PSP OR PSP)
    set(TARGET_PSP ON)
    set(TARGET_PS2 OFF)
    message(STATUS "Platform: PSP (Allegrex)")
elseif(DEFINED PS2SDK)
    set(TARGET_PS2 ON)
    set(TARGET_PSP OFF)
    message(STATUS "Platform: PS2 (R5900)")
else()
    message(FATAL_ERROR "Unknown platform. PS2SDK or PLATFORM_PSP must be defined by toolchain.")
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
option(ENABLE_TEX_DEBUG "Enable texture debug overlay (colored bounding boxes)" OFF)
option(HEADLESS "Build without GPU/video output (no-op GPU stubs)" OFF)

# PS2-only options
if(TARGET_PS2)
    option(ENABLE_PSX_TLB "Enable TLB fast-path for PSX RAM (experimental)" OFF)
    option(ENABLE_VU0_MICRO "Use VU0 micro mode for GTE acceleration" ON)
    option(ENABLE_MDEC_IPU "Use PS2 IPU hardware for MDEC IDCT+YUV→RGB" OFF)
endif()

# PSP-only options
if(TARGET_PSP)
    option(ENABLE_PSP_STRIDE_HACK "Zero-copy T4/T8 textures from EDRAM via stride trick" OFF)
    option(ENABLE_MDEC_VFPU "Use PSP VFPU for MDEC IDCT+CSC (fast, PPSSPP compatible)" ON)
endif()

# ============================================================================
# Source files
# ============================================================================
if(HEADLESS)
    set(GPU_SOURCES src/gpu_stub.c)
elseif(TARGET_PSP)
    set(GPU_SOURCES
        src/platform/psp/gpu_psp_core.c
        src/platform/psp/gpu_psp_vram.c
        src/platform/psp/gpu_psp_texture.c
        src/platform/psp/gpu_psp_primitives.c
        src/platform/psp/gpu_psp_dma.c
        src/platform/psp/gpu_psp_backend.c
        src/gpu_commands.c
        src/platform/psp/osd_psp.c
    )
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

# Platform-specific sources
if(TARGET_PSP)
    set(PLATFORM_SOURCES
        src/platform/psp/main_psp.c
        src/platform/psp/joystick_psp.c
        src/platform/psp/platform_psp.c
        src/platform/psp/audio_psp_backend.c
        src/platform/psp/vu0_micro_psp.c
        src/platform/psp/gte_vfpu.c
    )
    set(PLATFORM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/platform/psp)
else()
    set(PLATFORM_SOURCES
        src/platform/ps2/main_ps2.c
        src/platform/ps2/joystick_ps2.c
        src/platform/ps2/platform_ps2.c
        src/platform/ps2/audio_ps2_backend.c
        src/platform/ps2/tlb_handler_ps2.c
        src/platform/ps2/vu0_micro_ps2.c
    )
    set(PLATFORM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/platform/ps2)
endif()

set(SUPERPSX_SOURCES
    ${PLATFORM_SOURCES}
    src/init.c
    src/config.c
    src/memory.c
    src/cpu.c
    src/hardware.c
    src/dma.c
    src/mdec.c
    src/memorycard.c
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
    src/spu.c
    src/scheduler.c
    src/iso_image.c
    src/iso_fs.c
    src/profiler.c
    src/interpreter.c
)

# Platform-specific MDEC backends
if(TARGET_PS2)
    list(APPEND SUPERPSX_SOURCES src/mdec_ipu.c)
endif()
if(TARGET_PSP)
    list(APPEND SUPERPSX_SOURCES src/mdec_vfpu.c)
endif()

# ============================================================================
# VU0 Micro Program Assembly Pipeline (PS2 only)
# .vsm → dvp-as → .o → ee-objcopy (.vutext→.vudata) → link directly into ELF
# ============================================================================
set(VU0_OBJECTS)
if(TARGET_PS2)
    find_program(DVP_AS dvp-as PATHS "$ENV{PS2DEV}/dvp/bin" REQUIRED)
    find_program(EE_OBJCOPY mips64r5900el-ps2-elf-objcopy PATHS "$ENV{PS2DEV}/ee/bin" REQUIRED)

    set(VU0_SRC_DIR "${CMAKE_SOURCE_DIR}/src/platform/ps2/vu0")
    set(VU0_GEN_DIR "${CMAKE_BINARY_DIR}/generated/vu0")
    file(MAKE_DIRECTORY ${VU0_GEN_DIR})

    set(VU0_PROGRAMS mvmva_full mvmva_core)

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
endif()

# ============================================================================
# Executable target
# ============================================================================
if(TARGET_PSP)
    set(EXE_SUFFIX ".elf")
    set(PLATFORM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/platform/psp)
    set(PLATFORM_DEFINE PLATFORM_PSP)
else()
    set(EXE_SUFFIX ".elf")
    set(PLATFORM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/platform/ps2)
    set(PLATFORM_DEFINE PLATFORM_PS2)
endif()

add_executable(${PROJECT_NAME}${EXE_SUFFIX} ${SUPERPSX_SOURCES})

target_include_directories(${PROJECT_NAME}${EXE_SUFFIX} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PLATFORM_INCLUDE_DIR}
)

target_compile_definitions(${PROJECT_NAME}${EXE_SUFFIX} PRIVATE ${PLATFORM_DEFINE})

# ============================================================================
# Compile definitions based on options
# ============================================================================
set(MAIN_TARGET ${PROJECT_NAME}${EXE_SUFFIX})

if(ENABLE_VRAM_DUMP)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_VRAM_DUMP)
endif()

if(ENABLE_HOST_LOG)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_HOST_LOG)
endif()

if(ENABLE_DEBUG_LOG)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_DEBUG_LOG)
endif()

if(ENABLE_STUCK_DETECTION)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_STUCK_DETECTION)
endif()

if(ENABLE_DYNAREC_STATS)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_DYNAREC_STATS)
endif()

if(ENABLE_SUBSYSTEM_PROFILER)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_SUBSYSTEM_PROFILER)
endif()

if(ENABLE_PSX_TLB)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_PSX_TLB)
endif()

if(ENABLE_TEX_DEBUG)
    target_compile_definitions(${MAIN_TARGET} PRIVATE TEX_DEBUG_OVERLAY)
endif()

if(ENABLE_VU0_MICRO)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_VU0_MICRO)
    add_dependencies(${MAIN_TARGET} vu0_programs)
    target_link_libraries(${MAIN_TARGET} PRIVATE ${VU0_OBJECTS})
endif()

if(ENABLE_MDEC_IPU)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_MDEC_IPU)
endif()

if(ENABLE_PSP_STRIDE_HACK)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_PSP_STRIDE_HACK)
endif()

if(ENABLE_MDEC_VFPU)
    target_compile_definitions(${MAIN_TARGET} PRIVATE ENABLE_MDEC_VFPU)
endif()

if(HEADLESS)
    target_compile_definitions(${MAIN_TARGET} PRIVATE HEADLESS)
endif()

# ============================================================================
# Compiler flags
# ============================================================================
if(TARGET_PSP)
    target_compile_options(${MAIN_TARGET} PRIVATE -O2 -G0 -Wall -ffast-math)
else()
    target_compile_options(${MAIN_TARGET} PRIVATE -O2 -G0 -Wall)
endif()

if(ENABLE_PROFILING)
    target_compile_options(${MAIN_TARGET} PRIVATE -pg -g)
    target_link_options(${MAIN_TARGET} PRIVATE -pg -g)
endif()

if(ENABLE_LTO)
    target_compile_options(${MAIN_TARGET} PRIVATE -flto)
    target_link_options(${MAIN_TARGET} PRIVATE -flto)
endif()

# ============================================================================
# Link libraries
# ============================================================================
if(TARGET_PSP)
    target_link_libraries(${MAIN_TARGET} PRIVATE
        -lm
        -lc
        -lpspgu
        -lpspge
        -lpspdisplay
        -lpspaudio
        -lpspctrl
        -lpspdebug
        -lpsppower
        -lpspkernel
    )

    # Generate EBOOT.PBP from the ELF
    if(COMMAND create_pbp_file)
        create_pbp_file(
            TARGET ${MAIN_TARGET}
            TITLE "SuperPSX"
        )
    endif()
else()
    target_link_libraries(${MAIN_TARGET} PRIVATE
        -lpatches
        -lps2_drivers
        -lgraph
        -ldma
        -ldraw
        -lmath3d
    )
endif()

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
# Emulator runner settings (PCSX2 for PS2, PPSSPP for PSP)
# ============================================================================
if(TARGET_PSP)
    if(CMAKE_HOST_APPLE)
        set(PPSSPP_DEFAULT_PATH "/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL")
    else()
        set(PPSSPP_DEFAULT_PATH "ppsspp")
    endif()
    set(PPSSPP_PATH "${PPSSPP_DEFAULT_PATH}" CACHE FILEPATH "Path to PPSSPP executable")

    set(EMU_CMD "${PPSSPP_PATH}")
    list(APPEND EMU_CMD "${CMAKE_CURRENT_BINARY_DIR}/EBOOT.PBP")
    string(REPLACE ";" "\\;" EMU_CMD_ESC "${EMU_CMD}")
    set(EMU_RUN_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/run_ppsspp.cmake")
else()
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

    set(EMU_CMD "${PCSX2_PATH}")
    foreach(_f ${PCSX2_RUN_FLAGS})
        list(APPEND EMU_CMD "${_f}")
    endforeach()
    list(APPEND EMU_CMD -elf "${CMAKE_CURRENT_BINARY_DIR}/${MAIN_TARGET}")
    string(REPLACE ";" "\\;" EMU_CMD_ESC "${EMU_CMD}")
    set(EMU_RUN_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/run_pcsx2.cmake")
endif()

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
    src/interpreter.c
)

if(TARGET_PSP)
    list(APPEND PLAYGROUND_SOURCES
        src/platform/psp/platform_psp.c
        src/platform/psp/vu0_micro_psp.c
        src/platform/psp/gte_vfpu.c
        src/platform/psp/gpu_psp_backend.c
    )
else()
    list(APPEND PLAYGROUND_SOURCES
        src/platform/ps2/tlb_handler_ps2.c
        src/platform/ps2/vu0_micro_ps2.c
        src/platform/ps2/platform_ps2.c
        tests/jit/test_vu0_micro.c
        tests/jit/test_gte_compare.c
    )
endif()

add_executable(jit_playground.elf EXCLUDE_FROM_ALL ${PLAYGROUND_SOURCES})

target_include_directories(jit_playground.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${PLATFORM_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/tests/jit
)

# ============================================================================
# INT Playground — separate ELF for testing the interpreter in isolation
# ============================================================================
set(INT_PLAYGROUND_SOURCES
    tests/int/playground_main.c
    tests/int/playground_tests.c
    tests/int/test_alu.c
    tests/int/test_memory.c
    tests/int/test_branch.c
    tests/int/test_gte.c
    src/cpu.c
    src/memory.c
    src/scheduler.c
    src/gte.c
    src/interpreter.c
    src/dynarec_emit.c
    src/dynarec_cache.c
    src/dynarec_memory.c
    src/dynarec_compile.c
    src/dynarec_insn.c
    src/dynarec_run.c
)

if(TARGET_PSP)
    list(APPEND INT_PLAYGROUND_SOURCES
        src/platform/psp/platform_psp.c
        src/platform/psp/vu0_micro_psp.c
        src/platform/psp/gte_vfpu.c
        src/platform/psp/gpu_psp_backend.c
    )
else()
    list(APPEND INT_PLAYGROUND_SOURCES
        src/platform/ps2/tlb_handler_ps2.c
        src/platform/ps2/vu0_micro_ps2.c
        src/platform/ps2/platform_ps2.c
    )
endif()

add_executable(int_playground.elf EXCLUDE_FROM_ALL ${INT_PLAYGROUND_SOURCES})

target_include_directories(int_playground.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${PLATFORM_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/tests/int
)

if(TARGET_PSP)
    target_compile_options(int_playground.elf PRIVATE -O2 -G0 -Wall -ffast-math)
    target_compile_definitions(int_playground.elf PRIVATE INT_PLAYGROUND PLATFORM_PSP)
    target_link_libraries(int_playground.elf PRIVATE
        -lpsppower
        -lm
        -lc
        -lpspdebug
        -lpspkernel
    )
    # Fix up import stubs (must run after linking)
    add_custom_command(
        TARGET int_playground.elf
        POST_BUILD COMMAND
        "$ENV{PSPDEV}/bin/psp-fixup-imports" "$<TARGET_FILE:int_playground.elf>"
        COMMENT "Calling psp-fixup-imports for int_playground.elf"
    )
else()
    target_compile_options(int_playground.elf PRIVATE -O2 -G0 -Wall)
    target_compile_definitions(int_playground.elf PRIVATE INT_PLAYGROUND ENABLE_VU0_MICRO PLATFORM_PS2)
    add_dependencies(int_playground.elf vu0_programs)
    target_link_libraries(int_playground.elf PRIVATE
        ${VU0_OBJECTS}
        -lpatches
        -lps2_drivers
    )
endif()

if(TARGET_PSP)
    target_compile_options(jit_playground.elf PRIVATE -O2 -G0 -Wall -ffast-math)
    target_compile_definitions(jit_playground.elf PRIVATE JIT_PLAYGROUND PLATFORM_PSP)
    target_link_libraries(jit_playground.elf PRIVATE
        -lpsppower
        -lm
        -lc
        -lpspdebug
        -lpspkernel
    )
    # Fix up import stubs (must run after linking)
    add_custom_command(
        TARGET jit_playground.elf
        POST_BUILD COMMAND
        "$ENV{PSPDEV}/bin/psp-fixup-imports" "$<TARGET_FILE:jit_playground.elf>"
        COMMENT "Calling psp-fixup-imports for jit_playground.elf"
    )
else()
    target_compile_options(jit_playground.elf PRIVATE -O2 -G0 -Wall)
    target_compile_definitions(jit_playground.elf PRIVATE JIT_PLAYGROUND ENABLE_VU0_MICRO PLATFORM_PS2)
    add_dependencies(jit_playground.elf vu0_programs)
    target_link_libraries(jit_playground.elf PRIVATE
        ${VU0_OBJECTS}
        -lpatches
        -lps2_drivers
    )
endif()

# ============================================================================
# Generic "run" target — pass GAMEARGS at make-time:
#   make run GAMEARGS=tests/gpu/triangle/triangle.exe
# ============================================================================
if(TARGET_PSP)
    add_custom_target(run
        COMMAND ${CMAKE_COMMAND}
            "-DPPSSPP_CMD=${EMU_CMD_ESC}"
            -P "${EMU_RUN_SCRIPT}"
        DEPENDS ${MAIN_TARGET}
        COMMENT "Running ${MAIN_TARGET} in PPSSPP"
        VERBATIM
    )
else()
    add_custom_target(run
        COMMAND ${CMAKE_COMMAND}
            "-DPCSX2_CMD=${EMU_CMD_ESC}"
            -P "${EMU_RUN_SCRIPT}"
        DEPENDS ${MAIN_TARGET}
        COMMENT "Running ${MAIN_TARGET} in PCSX2"
        VERBATIM
    )
endif()

# Build emulator command for running the playground ELF
if(TARGET_PSP)
    set(PG_EMU_CMD "${PPSSPP_PATH}")
    list(APPEND PG_EMU_CMD "${CMAKE_CURRENT_BINARY_DIR}/jit_playground.elf")
    string(REPLACE ";" "\\;" PG_EMU_CMD_ESC "${PG_EMU_CMD}")

    add_custom_target(run-playground
        COMMAND ${CMAKE_COMMAND}
            "-DPPSSPP_CMD=${PG_EMU_CMD_ESC}"
            -P "${EMU_RUN_SCRIPT}"
        DEPENDS jit_playground.elf
        COMMENT "Running JIT Playground in PPSSPP"
        VERBATIM
    )
else()
    set(PCSX2_PG_CMD "${PCSX2_PATH}")
    foreach(_f ${PCSX2_RUN_FLAGS})
        list(APPEND PCSX2_PG_CMD "${_f}")
    endforeach()
    list(APPEND PCSX2_PG_CMD -elf "${CMAKE_CURRENT_BINARY_DIR}/jit_playground.elf")
    string(REPLACE ";" "\\;" PCSX2_PG_CMD_ESC "${PCSX2_PG_CMD}")

    add_custom_target(run-playground
        COMMAND ${CMAKE_COMMAND}
            "-DPCSX2_CMD=${PCSX2_PG_CMD_ESC}"
            -P "${EMU_RUN_SCRIPT}"
        DEPENDS jit_playground.elf
        COMMENT "Running JIT Playground in PCSX2"
        VERBATIM
    )

    set(INT_PG_CMD "${PCSX2_PATH}")
    foreach(_f ${PCSX2_RUN_FLAGS})
        list(APPEND INT_PG_CMD "${_f}")
    endforeach()
    list(APPEND INT_PG_CMD -elf "${CMAKE_CURRENT_BINARY_DIR}/int_playground.elf")
    string(REPLACE ";" "\\;" INT_PG_CMD_ESC "${INT_PG_CMD}")

    add_custom_target(run-int-playground
        COMMAND ${CMAKE_COMMAND}
            "-DPCSX2_CMD=${INT_PG_CMD_ESC}"
            -P "${EMU_RUN_SCRIPT}"
        DEPENDS int_playground.elf
        COMMENT "Running INT Playground in PCSX2"
        VERBATIM
    )
endif()

# ============================================================================
# GTE Playground — C vs VFPU comparison (PSP only)
# ============================================================================
if(TARGET_PSP)
set(GTE_PLAYGROUND_SOURCES
    tests/gte_playground/gte_playground_main.c
    tests/gte_playground/test_all_data.c
    src/gte.c
    src/platform/psp/gte_vfpu.c
)

add_executable(gte_playground.elf EXCLUDE_FROM_ALL ${GTE_PLAYGROUND_SOURCES})

target_include_directories(gte_playground.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/platform/psp
    ${CMAKE_SOURCE_DIR}/tests/gte_playground
)

target_compile_options(gte_playground.elf PRIVATE -O2 -G0 -Wall -ffast-math)
target_compile_definitions(gte_playground.elf PRIVATE GTE_PLAYGROUND PLATFORM_PSP)

target_link_libraries(gte_playground.elf PRIVATE
    -lm
    -lc
    -lpspdebug
    -lpspdisplay
    -lpspge
    -lpspkernel
)

# Fix up import stubs
add_custom_command(
    TARGET gte_playground.elf
    POST_BUILD COMMAND
    "$ENV{PSPDEV}/bin/psp-fixup-imports" "$<TARGET_FILE:gte_playground.elf>"
    COMMENT "Calling psp-fixup-imports for gte_playground.elf"
)

# Run target
set(GTE_PG_EMU_CMD "${PPSSPP_PATH}")
list(APPEND GTE_PG_EMU_CMD "${CMAKE_CURRENT_BINARY_DIR}/gte_playground.elf")
string(REPLACE ";" "\\;" GTE_PG_EMU_CMD_ESC "${GTE_PG_EMU_CMD}")

add_custom_target(run-gte-playground
    COMMAND ${CMAKE_COMMAND}
        "-DPPSSPP_CMD=${GTE_PG_EMU_CMD_ESC}"
        -P "${EMU_RUN_SCRIPT}"
    DEPENDS gte_playground.elf
    COMMENT "Running GTE Playground in PPSSPP"
    VERBATIM
)
endif() # TARGET_PSP (GTE Playground)

# ============================================================================
# GPU Playground — measure expansion of GPU commands (PS2 only)
# ============================================================================
if(TARGET_PS2)
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
endif() # TARGET_PS2 (GPU Playground)

# ============================================================================
# Print configuration summary
# ============================================================================
message(STATUS "")
message(STATUS "superpsx configuration:")
message(STATUS "  Version:              ${PROJECT_VERSION}")
if(TARGET_PSP)
    message(STATUS "  Platform:             PSP (Allegrex)")
else()
    message(STATUS "  Platform:             PS2 (R5900)")
endif()
message(STATUS "  ENABLE_VRAM_DUMP:     ${ENABLE_VRAM_DUMP}")
message(STATUS "  ENABLE_HOST_LOG:      ${ENABLE_HOST_LOG}")
message(STATUS "  ENABLE_DEBUG_LOG:     ${ENABLE_DEBUG_LOG}")
message(STATUS "  ENABLE_STUCK_DETECTION: ${ENABLE_STUCK_DETECTION}")
message(STATUS "  ENABLE_PROFILING:      ${ENABLE_PROFILING}")
message(STATUS "  ENABLE_LTO:            ${ENABLE_LTO}")
if(TARGET_PS2)
    message(STATUS "  ENABLE_UNLIMITED_SPEED: ${ENABLE_UNLIMITED_SPEED}")
endif()
message(STATUS "  ENABLE_DYNAREC_STATS:  ${ENABLE_DYNAREC_STATS}")
message(STATUS "  ENABLE_SUBSYSTEM_PROFILER: ${ENABLE_SUBSYSTEM_PROFILER}")
message(STATUS "  HEADLESS:              ${HEADLESS}")
message(STATUS "")
message(STATUS "  Usage:")
message(STATUS "    cmake -B build && cmake --build build")
if(TARGET_PSP)
    message(STATUS "    make -C build run")
else()
    message(STATUS "    make -C build run GAMEARGS=tests/gpu/triangle/triangle.exe")
endif()
message(STATUS "")

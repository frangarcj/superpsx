# Helper script invoked by the "run" target for PSP builds.
# Reads GAMEARGS from the environment so it can be set at make-time:
#   make run GAMEARGS=tests/gpu/triangle/triangle.exe

if(DEFINED ENV{GAMEARGS})
    set(GAMEARGS "$ENV{GAMEARGS}")
endif()

set(CMD ${PPSSPP_CMD})

# PPSSPP runs as a GUI app. For automated tests, use interpreter mode (-i)
# and verbose logging (-v) to capture printf output as I[PRINTF] lines.
# The PSP program exits via sceKernelExitGame when main() returns.

execute_process(COMMAND ${CMD} COMMAND_ERROR_IS_FATAL ANY)

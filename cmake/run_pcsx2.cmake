# Helper script invoked by the "run" target.
# Reads GAMEARGS from the environment so it can be set at make-time:
#   make run GAMEARGS=tests/gpu/triangle/triangle.exe

if(DEFINED ENV{GAMEARGS})
    set(GAMEARGS "$ENV{GAMEARGS}")
endif()

set(CMD ${PCSX2_CMD})
if(NOT "${GAMEARGS}" STREQUAL "")
    list(APPEND CMD -gameargs "host ${GAMEARGS}")
endif()

execute_process(COMMAND ${CMD} COMMAND_ERROR_IS_FATAL ANY)

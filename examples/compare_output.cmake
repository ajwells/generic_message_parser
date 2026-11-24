# cmake -DPROGRAM=<cmd;args> -DEXPECTED=<file> [-DWORKDIR=<dir>] -P compare_output.cmake
# Runs an example program and fails unless its standard output equals EXPECTED. Carriage returns are removed from
# both (C stdio on Windows writes \r\n; Node.js writes \n).
if (NOT WORKDIR)
    set(WORKDIR .)
endif ()
execute_process(COMMAND ${PROGRAM} WORKING_DIRECTORY "${WORKDIR}" OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
file(READ "${EXPECTED}" want)
string(REPLACE "\r" "" out "${out}")
string(REPLACE "\r" "" want "${want}")
if (NOT rc EQUAL 0)
    message(FATAL_ERROR "exit code ${rc}\n--- stderr:\n${err}\n--- stdout:\n${out}")
endif ()
if (NOT out STREQUAL want)
    message(FATAL_ERROR "output differs from ${EXPECTED}\n--- got:\n${out}\n--- want:\n${want}")
endif ()

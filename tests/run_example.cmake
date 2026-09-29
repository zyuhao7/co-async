# Runs one example under a time limit and checks how it terminated against the
# expectation recorded for it in tests/CMakeLists.txt.
#
# Most examples do their work and return. The servers, and the examples that
# wait for input this host cannot supply, are supposed to keep running until the
# limit kills them. "either" accepts both endings, for the example whose exit
# depends on the environment rather than on itself.
#
# Variables: NAME, EXECUTABLE, LIMIT (seconds), EXPECT (exit-zero|timeout|either)

execute_process(
    COMMAND "${EXECUTABLE}"
    INPUT_FILE /dev/null
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE res
    TIMEOUT "${LIMIT}")

string(SUBSTRING "${out}${err}" 0 300 tail)

if (res STREQUAL "Process terminated due to timeout")
    if (EXPECT STREQUAL "exit-zero")
        message(FATAL_ERROR "${NAME}: expected to finish, but it was still running after ${LIMIT}s\n${tail}")
    endif()
else()
    if (NOT res EQUAL 0)
        message(FATAL_ERROR "${NAME}: exited ${res}\n${tail}")
    endif()
    if (EXPECT STREQUAL "timeout")
        message(FATAL_ERROR "${NAME}: expected to keep running, but it exited 0\n${tail}")
    endif()
endif()

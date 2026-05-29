if(NOT DEFINED PROGRAM)
  message(FATAL_ERROR "PROGRAM is required")
endif()

if(NOT DEFINED ARGS)
  set(ARGS "")
endif()

execute_process(
  COMMAND "${PROGRAM}" ${ARGS}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

set(output "${stdout}${stderr}")

if(result EQUAL 0)
  message(FATAL_ERROR "expected command to fail, but it exited 0\n${output}")
endif()

if(DEFINED EXPECT_OUTPUT AND NOT output MATCHES "${EXPECT_OUTPUT}")
  message(FATAL_ERROR "expected output to match '${EXPECT_OUTPUT}'\n${output}")
endif()

if(DEFINED REJECT_OUTPUT AND output MATCHES "${REJECT_OUTPUT}")
  message(FATAL_ERROR "output unexpectedly matched '${REJECT_OUTPUT}'\n${output}")
endif()

message("${output}")

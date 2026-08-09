set(GATEWAY_TSAN_TEST_EXECUTOR "")

if(
  GATEWAY_ENABLE_TSAN
  AND GATEWAY_ENABLE_WSL_TSAN_WORKAROUND
  AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
)
  set(gateway_kernel_release "")
  if(EXISTS "/proc/sys/kernel/osrelease")
    file(READ "/proc/sys/kernel/osrelease" gateway_kernel_release LIMIT 256)
  endif()

  if(gateway_kernel_release MATCHES "[Mm]icrosoft.*WSL2")
    find_program(GATEWAY_SETARCH_EXECUTABLE NAMES setarch REQUIRED)
    set(
      GATEWAY_TSAN_TEST_EXECUTOR
      "${GATEWAY_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R"
    )
    message(
      STATUS
      "WSL2 TSan runtime detected: tests will run through "
      "${GATEWAY_SETARCH_EXECUTABLE} ${CMAKE_SYSTEM_PROCESSOR} -R"
    )
  endif()
endif()

function(gateway_configure_tsan_test_runtime target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Unknown test target: ${target}")
  endif()

  if(NOT GATEWAY_TSAN_TEST_EXECUTOR)
    return()
  endif()

  set_property(
    TARGET "${target}"
    PROPERTY CROSSCOMPILING_EMULATOR "${GATEWAY_TSAN_TEST_EXECUTOR}"
  )
endfunction()

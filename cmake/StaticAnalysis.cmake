function(gateway_enable_clang_tidy target)
  if(NOT GATEWAY_ENABLE_CLANG_TIDY)
    return()
  endif()

  find_program(GATEWAY_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set_target_properties(
    ${target}
    PROPERTIES CXX_CLANG_TIDY "${GATEWAY_CLANG_TIDY_EXECUTABLE}"
  )
endfunction()

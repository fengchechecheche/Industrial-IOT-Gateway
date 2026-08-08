function(gateway_enable_sanitizers target)
  if(GATEWAY_ENABLE_SANITIZERS AND GATEWAY_ENABLE_TSAN)
    message(FATAL_ERROR "ASan/UBSan and TSan must use separate build directories")
  endif()

  if(NOT GATEWAY_ENABLE_SANITIZERS AND NOT GATEWAY_ENABLE_TSAN)
    return()
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "The configured sanitizer profile requires GCC or Clang")
  endif()

  if(GATEWAY_ENABLE_SANITIZERS)
    target_compile_options(
      ${target}
      INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${target} INTERFACE -fsanitize=address,undefined)
  endif()

  if(GATEWAY_ENABLE_TSAN)
    target_compile_options(${target} INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} INTERFACE -fsanitize=thread)
  endif()
endfunction()

cmake_minimum_required(VERSION 3.10)

find_package(Threads REQUIRED)

function(add_pty_native_test test_name source_file)
  add_executable(${test_name} ${source_file} ${ARGN})
  target_compile_features(${test_name} PRIVATE c_std_11)
  target_include_directories(${test_name} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/include"
  )
  if (MSVC)
    target_compile_options(${test_name} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${test_name} PRIVATE -UNDEBUG)
  endif()
  target_link_libraries(${test_name} PRIVATE Threads::Threads)
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(${test_name} PRIVATE util)
  endif()
  set_target_properties(${test_name} PROPERTIES
    BUILD_RPATH "$<TARGET_FILE_DIR:flutter_pty2>"
  )
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
  if (test_name STREQUAL "pty_unix_session_stress_test")
    set_tests_properties(${test_name} PROPERTIES
      ENVIRONMENT "PTY_NATIVE_STRESS_CYCLES=1000"
    )
    if (APPLE)
      target_link_libraries(${test_name} PRIVATE proc)
    endif()
  endif()
endfunction()

if (WIN32)
  add_pty_native_test(
    pty_windows_environment_test
    pty_windows_environment_test.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_environment.c"
  )
  add_pty_native_test(
    pty_windows_commandline_test
    pty_windows_commandline_test.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_commandline.c"
  )
  add_pty_native_test(
    pty_windows_session_test
    pty_windows_session_test.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/flutter_pty.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_commandline.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_environment.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_error.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_event.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_refcount.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_debug_stats.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_write_queue.c"
  )
  add_pty_native_test(
    pty_windows_session_stress_test
    pty_windows_session_stress_test.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/flutter_pty.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_commandline.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_environment.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_error.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_event.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_refcount.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_debug_stats.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_write_queue.c"
  )
  add_pty_native_test(
    pty_event_failure_test
    pty_event_failure_test.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/flutter_pty.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_commandline.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_environment.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_error.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_event.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_refcount.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_debug_stats.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_write_queue.c"
  )
else()
  add_pty_native_test(
    pty_write_queue_test
    pty_write_queue_test.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_debug_stats.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_write_queue.c"
  )
  add_pty_native_test(
    pty_unix_options_clone_test
    pty_unix_options_clone_test.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/unix/pty_unix_spawn.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_error.c"
  )
  add_pty_native_test(
    pty_unix_spawn_test
    pty_unix_spawn_test.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/unix/pty_unix_spawn.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_error.c"
  )
  add_pty_native_test(
    pty_unix_signal_state_test
    pty_unix_signal_state_test.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/unix/pty_unix_spawn.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_error.c"
  )
  set(PTY_SESSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/flutter_pty.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_error.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_event.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_refcount.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_debug_stats.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_write_queue.c"
  )
  add_pty_native_test(pty_unix_session_test pty_unix_session_test.c ${PTY_SESSION_SOURCES})
  add_pty_native_test(pty_unix_close_race_test pty_unix_close_race_test.c ${PTY_SESSION_SOURCES})
  add_pty_native_test(pty_unix_concurrent_spawn_test pty_unix_concurrent_spawn_test.c ${PTY_SESSION_SOURCES})
  add_pty_native_test(pty_unix_startup_close_test pty_unix_startup_close_test.c ${PTY_SESSION_SOURCES})
  add_pty_native_test(pty_unix_session_stress_test pty_unix_session_stress_test.c ${PTY_SESSION_SOURCES})
  add_pty_native_test(pty_unix_output_credit_test pty_unix_output_credit_test.c ${PTY_SESSION_SOURCES})
  add_pty_native_test(pty_event_failure_test pty_event_failure_test.c ${PTY_SESSION_SOURCES})
endif()

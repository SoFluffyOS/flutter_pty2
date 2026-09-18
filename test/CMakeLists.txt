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
  set(PTY_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/include/dart_api_dl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_error.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_event.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_refcount.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_debug_stats.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common/pty_write_queue.c"
  )
  set(PTY_PLATFORM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_commandline.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_environment.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_spawn.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/windows/pty_windows_session.c"
  )
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
    ${PTY_COMMON_SOURCES}
    ${PTY_PLATFORM_SOURCES}
  )
  add_pty_native_test(
    pty_windows_session_stress_test
    pty_windows_session_stress_test.c
    ${PTY_COMMON_SOURCES}
    ${PTY_PLATFORM_SOURCES}
  )
  set_tests_properties(pty_windows_session_stress_test PROPERTIES
    ENVIRONMENT "PTY_TEST_EXPECT_PSEUDO_CONSOLE_RELEASE=1"
  )
  add_test(
    NAME pty_windows_session_close_worker_stress_test
    COMMAND $<TARGET_FILE:pty_windows_session_stress_test>
  )
  set_tests_properties(pty_windows_session_close_worker_stress_test PROPERTIES
    ENVIRONMENT "PTY_TEST_FORCE_PSEUDO_CONSOLE_CLOSE_WORKER=1;PTY_TEST_EXPECT_PSEUDO_CONSOLE_RELEASE=0"
    TIMEOUT 300
  )
  add_pty_native_test(
    pty_event_failure_test
    pty_event_failure_test.c
    ${PTY_COMMON_SOURCES}
    ${PTY_PLATFORM_SOURCES}
  )
  add_pty_native_test(
    pty_windows_finalizer_test
    pty_windows_finalizer_test.c
    ${PTY_COMMON_SOURCES}
    ${PTY_PLATFORM_SOURCES}
  )
  set_tests_properties(pty_windows_finalizer_test PROPERTIES
    ENVIRONMENT "PTY_TEST_FORCE_CLOSE_THREAD_FAILURE=1"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/include/dart_api_dl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/unix/pty_unix_spawn.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/unix/pty_unix_session.c"
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

cmake_minimum_required(VERSION 3.10)

project(pty_test_child LANGUAGES C)

add_executable(pty_test_child main.c)

if (WIN32)
  target_compile_definitions(pty_test_child PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

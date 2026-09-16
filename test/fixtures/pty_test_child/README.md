# PTY test child

This small executable is shared by the PTY integration tests. It intentionally
uses a command-line protocol so the same child behavior can be exercised by
the current implementation and the clean-slate implementation.

Build it outside the source tree:

```sh
cmake -S test/fixtures/pty_test_child -B /tmp/pty_test_child-build
cmake --build /tmp/pty_test_child-build
```

Set `PTY_TEST_CHILD` to the resulting executable before running integration
tests. For example:

```sh
PTY_TEST_CHILD=/tmp/pty_test_child-build/pty_test_child flutter test
```

Supported commands are `echo`, `echo-binary`, `print-argv`, `print-env`,
`print-cwd`, `print-size`, `print-size-after`, `flood-output`, `slow-output`,
`slow-input`, `copy-input`, `slow-copy-input`, `exit`, `exit-after-output`,
`spawn-child`, `spawn-grandchild`, and `hold`. The `crash` command terminates
the child with `SIGSEGV` on POSIX systems.

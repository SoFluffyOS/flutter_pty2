# PTY benchmarks

These scripts measure the clean-slate API with the native fixture. Build the
fixture and native library first, then run a benchmark with the corresponding
environment variables:

```sh
cmake -S test/fixtures/pty_test_child -B /tmp/pty_test_child-build
cmake --build /tmp/pty_test_child-build

FLUTTER_PTY2_LIBRARY=/path/to/libflutter_pty2.dylib \
PTY_TEST_CHILD=/tmp/pty_test_child-build/pty_test_child \
dart run benchmark/output.dart
```

The scripts emit CSV rows with minimum, median, p95, and mean latency. Input
and output benchmarks also report mean MiB/s. `concurrency.dart` exercises 1,
10, 50, and 100 sessions and reports the current Dart process RSS after each
cohort.

Use `PTY_BENCHMARK_ITERATIONS` and `PTY_BENCHMARK_WARMUPS` to control the
sample count. The defaults are five measured samples and one warmup sample.

# flutter_pty2

[![pub package](https://img.shields.io/pub/v/flutter_pty2.svg)](https://pub.dev/packages/flutter_pty2)

`flutter_pty2` is a maintained fork of the original
[`flutter_pty`](https://pub.dev/packages/flutter_pty) package from
[`TerminalStudio/flutter_pty`](https://github.com/TerminalStudio/flutter_pty).
The original package is no longer maintained, so this fork continues the package
under a new pub package name.

This package provides a Flutter FFI pseudo-terminal implementation for spawning
and controlling terminal processes. The clean-slate 2.0 API is available
alongside the existing compatibility API; platform verification is tracked
separately for Windows and Android.

The package requires Dart 3 and Flutter 3.10 or newer.

## Platform


| Linux | macOS | Windows | Android |
| :---: | :---: | :-----: | :-----: |
|   ✔️   |   ✔️   |    🧪    |    ✔️    |

## Quick start

```dart
import 'package:flutter_pty2/flutter_pty2.dart';

final session = await Pty.spawn(
  const PtySpawnOptions(
    executable: '/bin/bash',
    arguments: ['-l'],
    size: PtySize(columns: 120, rows: 40),
  ),
);

session.output.listen((data) => ...);

await session.input.writeUtf8('ls -al\n');

session.resize(const PtySize(columns: 120, rows: 30));

final exit = await session.done;
await session.close();
```

`output` is a stream of raw `Uint8List` chunks. Output flow control is
automatic: the native backend stops reading when the bounded output window is
full and resumes as Dart consumes the stream. Input supports asynchronous
`write`, non-blocking `tryWrite`, and `flush`.

`processExit` completes when the child exits. `done` completes only after the
child has exited and PTY output reaches EOF. Always await `close()` when the
session is no longer needed.

### Input and lifecycle semantics

`input.write(bytes)` accepts arbitrary binary data and completes when the
native backend has written those bytes or fails with a typed exception. Use
`tryWrite(bytes)` for a non-blocking operation: it returns `accepted` when the
request was queued, `backpressured` when the bounded input queue is full, or
`closed` after the session has stopped accepting input. Accepted `tryWrite`
requests can be awaited together with `input.flush()`.

```dart
switch (session.input.tryWrite(bytes)) {
  case PtyWriteResult.accepted:
    await session.input.flush();
  case PtyWriteResult.backpressured:
    await session.input.write(bytes);
  case PtyWriteResult.closed:
    throw const PtyClosedException();
}
```

`close()` is idempotent and performs asynchronous native shutdown. It may
discard output that has not already been delivered to the output listener.
Use `done` when all output must be drained before cleanup. On Unix,
`sendSignal` targets the configured POSIX process or process group; signal
operations are unsupported on Windows. Windows uses ConPTY and Job Object
containment, while Unix process-tree termination is best effort and does not
guarantee cleanup of every detached descendant.

The existing `Pty.start` API remains available from
`package:flutter_pty2/flutter_pty.dart` as a compatibility layer. It uses the
legacy manual-acknowledgement semantics and is not the 2.0 API.

### Clean-slate backend status

Unix has the native async session, bounded input queue, output credits,
structured errors, and lifecycle stress coverage. Windows has the ConPTY,
Job Object, worker implementation, and scheduled native/integration stress
coverage, but requires runtime verification on Windows. Android has an
`arm64-v8a` NDK build path and has passed the clean-slate output and input
integration subset on an API 35 emulator. iOS clean-slate runtime support is
not yet claimed.

---

## Development

Install dependencies and run the Dart checks from this directory:

```sh
flutter pub get
dart format --set-exit-if-changed lib test benchmark tool
flutter analyze
flutter test
```

The unconfigured Dart suite runs all model and controller tests. Native
integration tests are skipped unless both the native library and fixture are
provided. Build them with CMake:

```sh
cmake -S src -B /tmp/flutter_pty2-native \
  -DFLUTTER_PTY2_BUILD_TESTS=ON
cmake --build /tmp/flutter_pty2-native
cmake -S test/fixtures/pty_test_child -B /tmp/flutter_pty2-fixture
cmake --build /tmp/flutter_pty2-fixture
```

On macOS, run the configured Unix integration suite with:

```sh
FLUTTER_PTY2_LIBRARY=/tmp/flutter_pty2-native/libflutter_pty2.dylib \
PTY_TEST_CHILD=/tmp/flutter_pty2-fixture/pty_test_child \
flutter test test/clean_slate_integration_test.dart
```

The Android clean-slate integration subset runs from the generated example app
so Flutter installs the FFI plugin into an emulator process:

```sh
cd example
flutter pub get
flutter test -d emulator-5554 \
  integration_test/clean_slate_android_integration_test.dart
```

The same example-app command is used by the scheduled Android emulator job.

Use `libflutter_pty2.so` on Linux and `flutter_pty2.dll` on Windows. The
native CTest suite is available in the native build directory:

```sh
ctest --test-dir /tmp/flutter_pty2-native --output-on-failure
```

The generated clean-slate FFI bindings are checked in under
`lib/src/generated/`. Regenerate and verify them with:

```sh
dart run ffigen --config ffigen_v2.yaml
git diff --exit-code -- lib/src/generated/flutter_pty_bindings_generated.dart
```

## Benchmarks

The benchmark programs cover spawn and close latency, input and output
throughput, interactive latency, idle/loaded concurrency, live RSS, and
current-process CPU usage. Build the native library and fixture first, then
run a benchmark such as:

```sh
FLUTTER_PTY2_LIBRARY=/tmp/flutter_pty2-native/libflutter_pty2.dylib \
PTY_TEST_CHILD=/tmp/flutter_pty2-fixture/pty_test_child \
dart run benchmark/output.dart
```

Results are CSV rows with minimum, median, p95, mean latency, and throughput
where applicable. Set `PTY_BENCHMARK_ITERATIONS` and
`PTY_BENCHMARK_WARMUPS` to control sampling. The large-transfer integration
test defaults to 100 MiB and accepts `PTY_LARGE_TRANSFER_BYTES` for larger
scheduled runs.

## Native architecture

The clean-slate API uses one Dart receive port per session and a native
reference-counted session. Unix uses a poll-based reactor with bounded output
credit and input writes; Windows uses ConPTY with dedicated reader, writer,
waiter, and close workers. The finalizer only starts non-blocking native
cleanup; deterministic callers should still await `close()`. The detailed
ownership and platform design is documented in
[`doc/architecture.md`](doc/architecture.md).

The legacy `Pty.start` API remains available from
`package:flutter_pty2/flutter_pty.dart`. It is maintained for compatibility
and retains its manual output acknowledgement behavior; new code should use
`package:flutter_pty2/flutter_pty2.dart`.

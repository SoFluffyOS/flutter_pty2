## 2.0.0 - 2026-09-20

This is a breaking clean-slate release. The legacy `Pty.start`, boolean
`write`, manual `ackRead`, and `destroy` API has been removed.

### Release summary

* Add `Pty.spawn(PtySpawnOptions)` and `PtySession` with raw `Uint8List`
  output, asynchronous input, automatic output credits, bounded flow control,
  typed exceptions, POSIX signal targeting, and idempotent asynchronous close.
* Split `processExit` from `done`; `done` completes only after the child exits
  and all PTY output has drained.
* Bound copied Dart input plus native queued and in-flight writes by
  `inputBufferBytes`, including arbitrarily large `write()` calls, and preserve
  real native `tryWrite` failures.
* Use a Unix poll reactor with standard terminal line discipline and inherited
  child signal reset before `execve`.
* Use Windows ConPTY worker ownership with Job Object containment, dynamic
  `ReleasePseudoConsole` support, and an older-Windows close-worker fallback.
* Add lifecycle, binary-transfer, exit-drain, resize, signal, finalizer,
  process-tree, failure, leak, stress, native CTest, sanitizer, and analyzer
  release gates across supported platform workflows.

### Migration guide

#### Imports and spawning

Replace the removed entry point and synchronous constructor:

```dart
// 1.x
import 'package:flutter_pty2/flutter_pty.dart';

final pty = Pty.start(
  '/bin/bash',
  arguments: ['-l'],
  columns: 120,
  rows: 40,
);
```

with the 2.0 session API:

```dart
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';

final session = await Pty.spawn(
  const PtySpawnOptions(
    executable: '/bin/bash',
    arguments: ['-l'],
    size: PtySize(columns: 120, rows: 40),
  ),
);
```

#### Input and output

`output` now emits raw `Uint8List` chunks. `write()` is asynchronous and no
longer returns a boolean. Use `tryWrite()` when admission must not wait:

```dart
final bytes = Uint8List.fromList([0x03]);
await session.input.write(bytes);

switch (session.input.tryWrite(bytes)) {
  case PtyWriteResult.accepted:
    await session.input.flush();
  case PtyWriteResult.backpressured:
    await session.input.write(bytes);
  case PtyWriteResult.closed:
    throw const PtyClosedException();
}
```

Remove all calls to `ackRead`; output credits are managed automatically. Do
not mutate a buffer until its asynchronous `write()` future completes.

#### Resize, signals, and shutdown

```dart
session.resize(
  const PtySize(
    columns: 120,
    rows: 40,
    pixelWidth: 960,
    pixelHeight: 640,
  ),
);

session.kill();
await session.done;
await session.close();
```

Use `sendSignal()` with `PosixSignal` and a `PosixSignalTarget` when a
platform-supported POSIX signal is required. Replace `exitCode` with
`processExit` for child-exit timing, or `done` when trailing PTY output must
also be drained. Replace `destroy()` with awaited `close()`.

#### Environment and terminal identity

Use `PtyEnvironment.inherit()` for the current process environment,
`PtyEnvironment.inherit(overrides: ..., remove: ...)` for changes, or
`PtyEnvironment.replace(values)` for an explicit environment. Set
`TERM_PROGRAM_VERSION` through an environment override when shell integration
needs it.

## 1.0.2

* Prevent Unix PTY wake descriptors from leaking into child processes.

## 1.0.1

* Preserve trailing PTY output and reject writes after output closes.
* Surface resize and child startup failures reliably.
* Harden Unix environment, UTF-8 input, and concurrent native errors.
* Improve ConPTY shutdown and contain Windows process trees.

## 1.0.0

* Rename package to `flutter_pty2` for the maintained fork.
* Add explicit `TERM_PROGRAM_VERSION` support for shell integrations.
* Replace Unix cross-thread mutex unlocking with a poll/wakeup event loop.
* Drain PTY output before reporting process exit.
* Clean up native spawn allocations and reader thread resources.
* Handle partial and interrupted Unix writes.
* Queue Unix input through the nonblocking PTY event loop.
* Propagate terminal pixel dimensions during PTY resize.
* Report Unix child setup failures synchronously and close all forked PTY descriptors.
* Preserve the full process environment, advertise true color, enable `IUTF8`, and reset child signals.
* Harden ConPTY ownership, remove startup latency, and support quoted Unicode process arguments.
* Sanitize inherited terminal-emulator identity and provide a UTF-8 locale fallback.

## 0.4.2

* Fix Linux compile error, thanks [@mengyanshou].

## 0.4.1

* Fix compile warning, thanks [@mengyanshou].

## 0.4.0

* Update to Dart3

## 0.3.1

* Update deps

## 0.3.0

* Fixes ignored working directory parameter for Unix [#3], thanks [@devmil].
* Support setting Windows environmental variable and working directory.

## 0.2.0

* Add optional read acknowledge [#2], thanks [@devmil].

## 0.1.1

* Update README

## 0.1.0

* Windows support.
* Support getting exit code

## 0.0.7

* Work on Linux #1
* Work on Android

## 0.0.6

* Flutter >=2.12.0

## 0.0.5

* Fix README syntax

## 0.0.4

* Support resizing of the pty

## 0.0.3

* Support passing env vars

## 0.0.2

* Support passing arguments

## 0.0.1

* Initial release

[#2]: https://github.com/TerminalStudio/flutter_pty/pull/2
[#3]: https://github.com/TerminalStudio/flutter_pty/pull/3

[@devmil]: https://github.com/devmil
[@mengyanshou]: https://github.com/mengyanshou

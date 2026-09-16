# flutter_pty2

[![pub package](https://img.shields.io/pub/v/flutter_pty2.svg)](https://pub.dev/packages/flutter_pty2)

`flutter_pty2` is a maintained fork of the original
[`flutter_pty`](https://pub.dev/packages/flutter_pty) package from
[`TerminalStudio/flutter_pty`](https://github.com/TerminalStudio/flutter_pty).
The original package is no longer maintained, so this fork continues the package
under a new pub package name.

This package provides a Flutter FFI pseudo-terminal implementation for spawning
and controlling terminal processes. The clean-slate 2.0 API is being introduced
alongside the existing API while the native backends are completed.

## Platform


| Linux | macOS | Windows | Android |
| :---: | :---: | :-----: | :-----: |
|   ✔️   |   ✔️   |    🧪    |    🧪    |

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

The existing `Pty.start` API remains available from
`package:flutter_pty2/flutter_pty.dart` as a compatibility layer. It uses the
legacy manual-acknowledgement semantics and is not the 2.0 API.

### Clean-slate backend status

Unix has the native async session, bounded input queue, output credits,
structured errors, and lifecycle stress coverage. Windows has the ConPTY,
Job Object, and worker implementation plus a host smoke test, but requires
runtime verification on Windows. Android and iOS clean-slate runtime support
are not yet claimed.

---

## Project structure

This template uses the following structure:

* `src`: Contains the native source code, and a CmakeFile.txt file for building
  that source code into a dynamic library.

* `lib`: Contains the Dart code that defines the API of the plugin, and which
  calls into the native code using `dart:ffi`.

* platform folders (`android`, `ios`, `windows`, etc.): Contains the build files
  for building and bundling the native code library with the platform application.

## Building and bundling native code

The `pubspec.yaml` specifies FFI plugins as follows:

```yaml
  plugin:
    platforms:
      some_platform:
        ffiPlugin: true
```

This configuration invokes the native build for the various target platforms
and bundles the binaries in Flutter applications using these FFI plugins.

This can be combined with dartPluginClass, such as when FFI is used for the
implementation of one platform in a federated plugin:

```yaml
  plugin:
    implements: some_other_plugin
    platforms:
      some_platform:
        dartPluginClass: SomeClass
        ffiPlugin: true
```

A plugin can have both FFI and method channels:

```yaml
  plugin:
    platforms:
      some_platform:
        pluginClass: SomeName
        ffiPlugin: true
```

The native build systems that are invoked by FFI (and method channel) plugins are:

* For Android: Gradle, which invokes the Android NDK for native builds.
  * See the documentation in android/build.gradle.
* For iOS and macOS: Xcode, via Swift Package Manager or CocoaPods.
  * See `ios/flutter_pty2/Package.swift` and `ios/flutter_pty2.podspec`.
  * See `macos/flutter_pty2/Package.swift` and `macos/flutter_pty2.podspec`.
* For Linux and Windows: CMake.
  * See the documentation in linux/CMakeLists.txt.
  * See the documentation in windows/CMakeLists.txt.

## Binding to native code

To use the native code, bindings in Dart are needed.
To avoid writing these by hand, they are generated from the header file
(`src/flutter_pty.h`) by `package:ffigen`.
Regenerate the compatibility bindings with
`flutter pub run ffigen --config ffigen.yaml`. The clean-slate bindings use
`ffigen_v2.yaml` and are generated into `lib/src/generated/`.

## Invoking native code

Very short-running native functions can be directly invoked from any isolate.
For example, see `Pty.write` in `lib/flutter_pty.dart`.

Longer-running functions should be invoked on a helper isolate to avoid
dropping frames in Flutter applications.
For example, see the output stream handling in `lib/flutter_pty.dart`.

## Flutter help

For help getting started with Flutter, view our
[online documentation](https://flutter.dev/docs), which offers tutorials,
samples, guidance on mobile development, and a full API reference.

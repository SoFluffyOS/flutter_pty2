# flutter_pty2

[![pub package](https://img.shields.io/pub/v/flutter_pty2.svg)](https://pub.dev/packages/flutter_pty2)

`flutter_pty2` is a maintained fork of the original
[`flutter_pty`](https://pub.dev/packages/flutter_pty) package from
[`TerminalStudio/flutter_pty`](https://github.com/TerminalStudio/flutter_pty).
The original package is no longer maintained, so this fork continues the package
under a new pub package name.

This package provides a Flutter FFI pseudo-terminal implementation for spawning
and controlling terminal processes.

## Platform


| Linux | macOS | Windows | Android |
| :---: | :---: | :-----: | :-----: |
|   ✔️   |   ✔️   |    ✔️    |    ✔️    |

## Quick start

```dart
import 'package:flutter_pty2/flutter_pty2.dart';

final pty = Pty.start('bash');

pty.output.listen((data) => ...);

pty.write(Utf8Encoder().convert('ls -al\n'));

pty.resize(30, 80);

pty.kill();
```

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
* For iOS and MacOS: Xcode, via CocoaPods.
  * See the documentation in ios/flutter_pty2.podspec.
  * See the documentation in macos/flutter_pty2.podspec.
* For Linux and Windows: CMake.
  * See the documentation in linux/CMakeLists.txt.
  * See the documentation in windows/CMakeLists.txt.

## Binding to native code

To use the native code, bindings in Dart are needed.
To avoid writing these by hand, they are generated from the header file
(`src/flutter_pty.h`) by `package:ffigen`.
Regenerate the bindings by running `flutter pub run ffigen --config ffigen.yaml`.

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

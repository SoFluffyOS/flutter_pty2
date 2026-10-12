import 'dart:ffi';
import 'dart:io';

import 'package:flutter_pty2/src/internal/native_symbols.dart';

/// Finds a symbol of the native PTY library.
typedef PtySymbolLookup =
    Pointer<T> Function<T extends NativeType>(String symbolName);

/// Looks up symbols in the library that `hook/build.dart` bundles with the
/// app, or in the library at `FLUTTER_PTY2_LIBRARY` when that is set, such as
/// a library built by CMake for the native tests.
PtySymbolLookup openPtyLibrary() {
  final override = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  if (override case final path? when path.isNotEmpty) {
    return DynamicLibrary.open(path).lookup;
  }
  return lookupBundledSymbol;
}

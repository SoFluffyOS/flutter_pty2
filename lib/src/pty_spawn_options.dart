import 'package:flutter_pty2/src/pty_environment.dart';
import 'package:flutter_pty2/src/pty_session.dart';
import 'package:flutter_pty2/src/pty_size.dart';

final class PtySpawnOptions {
  const PtySpawnOptions({
    required this.executable,
    this.arguments = const [],
    this.workingDirectory,
    this.environment = const PtyEnvironment.inherit(),
    this.size = const PtySize(columns: 80, rows: 24),
    this.inputBufferBytes = 1024 * 1024,
    this.outputWindowBytes = 256 * 1024,
    this.outputCoalescing = defaultOutputCoalescing,
  });

  /// The default [outputCoalescing].
  static const defaultOutputCoalescing = Duration(milliseconds: 2);

  /// The longest [outputCoalescing] the native backend accepts.
  static const maxOutputCoalescing = Duration(milliseconds: 100);

  final String executable;
  final List<String> arguments;
  final String? workingDirectory;
  final PtyEnvironment environment;
  final PtySize size;

  /// Maximum native queued input and admitted Dart-side asynchronous input.
  final int inputBufferBytes;

  /// Maximum native output that may be outstanding before reading pauses.
  final int outputWindowBytes;

  /// How long output that arrives right after the previous output chunk may
  /// be held so that more of it goes into the same chunk. Unix only; Windows
  /// ignores it.
  ///
  /// A program that writes many small pieces, such as one line at a time
  /// through the terminal's line discipline, otherwise produces one chunk
  /// per few hundred bytes, and each chunk costs CPU on both the native
  /// reader and the Dart side. Output is held only while chunks follow each
  /// other within this time, and at most until this long after the previous
  /// chunk, so output after a pause (such as the echo of a keystroke) is
  /// delivered at once. Exit and [PtySession.close] end a hold immediately.
  ///
  /// [Duration.zero] turns coalescing off. The value is rounded down to
  /// whole microseconds and must not exceed [maxOutputCoalescing].
  final Duration outputCoalescing;

  void validate() {
    if (executable.isEmpty) {
      throw ArgumentError.value(executable, 'executable', 'cannot be empty');
    }
    if (executable.contains('\x00')) {
      throw ArgumentError.value(
        executable,
        'executable',
        'cannot contain NUL bytes',
      );
    }
    for (final argument in arguments) {
      if (argument.contains('\x00')) {
        throw ArgumentError.value(
          argument,
          'arguments',
          'cannot contain NUL bytes',
        );
      }
    }
    if (workingDirectory?.contains('\x00') ?? false) {
      throw ArgumentError.value(
        workingDirectory,
        'workingDirectory',
        'cannot contain NUL bytes',
      );
    }
    size.validate();
    environment.validate();
    if (inputBufferBytes < 64 * 1024) {
      throw ArgumentError.value(
        inputBufferBytes,
        'inputBufferBytes',
        'must be at least 65536 bytes',
      );
    }
    if (outputWindowBytes < 16 * 1024) {
      throw ArgumentError.value(
        outputWindowBytes,
        'outputWindowBytes',
        'must be at least 16384 bytes',
      );
    }
    if (outputCoalescing.isNegative || outputCoalescing > maxOutputCoalescing) {
      throw ArgumentError.value(
        outputCoalescing,
        'outputCoalescing',
        'must be between zero and $maxOutputCoalescing',
      );
    }
  }
}

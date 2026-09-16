import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

const _defaultTransferSize = 100 * 1024 * 1024;

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final fixture = Platform.environment['PTY_TEST_CHILD'];
  final transferSize = _configuredTransferSize();
  final configured =
      (Platform.isLinux || Platform.isMacOS || Platform.isWindows) &&
          library?.isNotEmpty == true &&
          fixture?.isNotEmpty == true;
  final skipReason = switch (configured) {
    true => null,
    _ => 'Set FLUTTER_PTY2_LIBRARY and PTY_TEST_CHILD on a desktop '
        'platform to run large-transfer tests.',
  };

  test(
    'round-trips an exact child-to-host binary stream while output is paused',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: ['flood-output', '$transferSize'],
          outputWindowBytes: 16 * 1024,
        ),
      );
      final outputDone = Completer<void>();
      var received = 0;
      var mismatched = false;
      final subscription = session.output.listen(
        (chunk) {
          for (var index = 0; index < chunk.length; index++) {
            if (chunk[index] != (received + index) % 251) {
              mismatched = true;
              break;
            }
          }
          received += chunk.length;
        },
        onDone: outputDone.complete,
      );
      try {
        subscription.pause();
        await Future<void>.delayed(const Duration(seconds: 3));
        subscription.resume();
        final exit = await session.done.timeout(const Duration(minutes: 5));
        await outputDone.future.timeout(const Duration(minutes: 5));

        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        expect(mismatched, isFalse);
        expect(received, transferSize);
      } finally {
        await subscription.cancel();
        await session.close();
      }
    },
    skip: skipReason,
    timeout: const Timeout(Duration(minutes: 12)),
  );

  test(
    'round-trips an exact host-to-child binary stream',
    () async {
      final child = fixture;
      if (child == null) return;
      const chunkSize = 64 * 1024;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: ['copy-input', '$transferSize'],
        ),
      );
      final outputDone = Completer<void>();
      var received = 0;
      var mismatched = false;
      final subscription = session.output.listen(
        (chunk) {
          for (var index = 0; index < chunk.length; index++) {
            if (chunk[index] != (received + index) % 251) {
              mismatched = true;
              break;
            }
          }
          received += chunk.length;
        },
        onDone: outputDone.complete,
      );
      try {
        for (var offset = 0; offset < transferSize;) {
          final length = math.min(chunkSize, transferSize - offset);
          final chunk = Uint8List(length);
          for (var index = 0; index < length; index++) {
            chunk[index] = (offset + index) % 251;
          }
          await session.input.write(chunk);
          offset += length;
        }

        final exit = await session.done.timeout(const Duration(minutes: 5));
        await outputDone.future.timeout(const Duration(minutes: 5));

        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        expect(mismatched, isFalse);
        expect(received, transferSize);
      } finally {
        await subscription.cancel();
        await session.close();
      }
    },
    skip: skipReason,
    timeout: const Timeout(Duration(minutes: 12)),
  );
}

int _configuredTransferSize() {
  final rawValue = Platform.environment['PTY_LARGE_TRANSFER_BYTES'];
  if (rawValue == null || rawValue.isEmpty) return _defaultTransferSize;

  final value = int.tryParse(rawValue);
  if (value == null || value < 1) {
    throw ArgumentError.value(
      rawValue,
      'PTY_LARGE_TRANSFER_BYTES',
      'must be a positive integer',
    );
  }
  return value;
}

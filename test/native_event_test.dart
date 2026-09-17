import 'dart:typed_data';

import 'package:flutter_pty2/src/internal/native_event.dart';
import 'package:flutter_pty2/src/pty_exception.dart';
import 'package:flutter_pty2/src/pty_exit.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('parses spawned and output events', () {
    final spawned = NativeEvent.parse(<Object?>[1, 42, 7]);
    final output = NativeEvent.parse(<Object?>[
      3,
      Uint8List.fromList([1, 2, 3]),
    ]);

    final spawnedEvent = spawned as NativeSpawned;
    expect(spawnedEvent.pid, 42);
    expect(spawnedEvent.capabilities, 7);
    expect((output as NativeOutput).bytes, [1, 2, 3]);
  });

  test('parses typed errors, async errors, and signal exits', () {
    final event = NativeEvent.parse(<Object?>[
      2,
      PtyErrorDomain.posix.index + 1,
      PtyErrorKind.notFound.index + 1,
      2,
      'missing executable',
    ]);
    final asyncError = NativeEvent.parse(<Object?>[
      9,
      PtyErrorDomain.win32.index + 1,
      PtyErrorKind.io.index + 1,
      109,
      'read failed',
    ]);
    final exit = NativeEvent.parse(<Object?>[8, 1, 15]);

    expect((event as NativeSpawnFailed).error.kind, PtyErrorKind.notFound);
    expect((asyncError as NativeAsyncError).error.kind, PtyErrorKind.io);
    final processExit = (exit as NativeProcessExit).exit;
    expect(processExit, isA<PtySignalExit>());
    expect((processExit as PtySignalExit).signal, 15);
  });

  test('rejects malformed and unknown events', () {
    expect(
      () => NativeEvent.parse(<Object?>[]),
      throwsFormatException,
    );
    expect(
      () => NativeEvent.parse(<Object?>[99]),
      throwsFormatException,
    );
    expect(
      () => NativeEvent.parse(<Object?>[
        3,
        [1, 2]
      ]),
      throwsFormatException,
    );
    expect(
      () => NativeEvent.parse(<Object?>[
        9,
        PtyErrorDomain.posix.index + 1,
        999,
        1,
        'bad kind',
      ]),
      throwsFormatException,
    );
    expect(
      () => NativeEvent.parse(<Object?>[1, 42, 7, 8]),
      throwsFormatException,
    );
    expect(
      () => NativeEvent.parse(<Object?>[4, 'unexpected']),
      throwsFormatException,
    );
  });
}

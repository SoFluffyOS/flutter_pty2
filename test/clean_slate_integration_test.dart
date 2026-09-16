import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final fixture = Platform.environment['PTY_TEST_CHILD'];
  final nativeConfigured = Platform.isMacOS && library?.isNotEmpty == true;
  final integrationConfigured = Platform.isMacOS &&
      library?.isNotEmpty == true &&
      fixture?.isNotEmpty == true;
  final nativeSkipReason = switch (nativeConfigured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY on macOS to run native integration tests.',
  };
  final skipReason = switch (integrationConfigured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY and PTY_TEST_CHILD on macOS to run '
        'native integration tests.',
  };
  test(
    'spawns a process and drains raw output through the clean-slate API',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/bin/sh',
          arguments: ['-c', 'printf clean-slate-integration; exit 13'],
        ),
      );
      final outputFuture = session.output.toList();
      final exit = await session.done;
      final output = await outputFuture;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) {
        expect(code, 13);
      }
      expect(
        output.expand((chunk) => chunk).toList(),
        'clean-slate-integration'.codeUnits,
      );
    },
    skip: skipReason,
  );

  test(
    'pausing output applies bounded native credit and resumes in order',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['flood-output', '1000000'],
          outputWindowBytes: 16 * 1024,
        ),
      );
      final chunks = <Uint8List>[];
      final subscription = session.output.listen(chunks.add);
      subscription.pause();
      await Future<void>.delayed(const Duration(milliseconds: 100));
      subscription.resume();
      final processExit = await session.processExit.timeout(
        const Duration(seconds: 5),
        onTimeout: () => throw StateError('flood process did not exit'),
      );
      await session.close();

      expect(processExit, isA<PtyExitCode>());
      if (processExit case PtyExitCode(:final code)) expect(code, 0);
      final bytes = chunks.expand((chunk) => chunk).toList();
      expect(bytes, List<int>.generate(1000000, (index) => index % 251));
    },
    skip: skipReason,
  );

  test(
    'writes asynchronous input and receives the terminal response',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/bin/sh',
          arguments: ['-c', 'IFS= read line; printf response:%s "\$line"'],
        ),
      );
      final outputFuture = session.output.toList();
      await session.input.writeUtf8('input-value\n');
      final exit = await session.done;
      final output = await outputFuture;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(
        output.expand((chunk) => chunk).toList(),
        containsAllInOrder('response:input-value'.codeUnits),
      );
    },
    skip: skipReason,
  );

  test(
    'keeps trailing output available after process exit until done',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['exit-after-output', '37', 'fixture-sentinel'],
        ),
      );
      final outputFuture = session.output.toList();

      final processExit = await session.processExit;
      expect(processExit, isA<PtyExitCode>());
      if (processExit case PtyExitCode(:final code)) expect(code, 37);

      final done = await session.done;
      expect(done, isA<PtyExitCode>());
      if (done case PtyExitCode(:final code)) expect(code, 37);
      final output = await outputFuture;
      await session.close();

      expect(
        output.expand((chunk) => chunk).toList(),
        'fixture-sentinel'.codeUnits,
      );
    },
    skip: skipReason,
  );

  test(
    'round-trips an exact 100 MiB binary stream',
    () async {
      final child = fixture;
      if (child == null) return;
      const transferSize = 100 * 1024 * 1024;
      const chunkSize = 64 * 1024;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['copy-input', '$transferSize'],
        ),
      );
      final outputDone = Completer<void>();
      var received = 0;
      var mismatched = false;
      session.output.listen(
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

      for (var offset = 0; offset < transferSize;) {
        final length = math.min(chunkSize, transferSize - offset);
        final chunk = Uint8List(length);
        for (var index = 0; index < length; index++) {
          chunk[index] = (offset + index) % 251;
        }
        await session.input.write(chunk);
        offset += length;
      }

      final exit = await session.done;
      await outputDone.future;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(mismatched, isFalse);
      expect(received, transferSize);
    },
    skip: skipReason,
    timeout: const Timeout(Duration(minutes: 2)),
  );

  test(
    'completes writes while the child reads slowly',
    () async {
      final child = fixture;
      if (child == null) return;
      const transferSize = 1024 * 1024;
      const chunkSize = 64 * 1024;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['slow-copy-input', '$transferSize', '1'],
          inputBufferBytes: 64 * 1024,
        ),
      );
      final outputDone = Completer<void>();
      var received = 0;
      var mismatched = false;
      session.output.listen(
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

      for (var offset = 0; offset < transferSize;) {
        final length = math.min(chunkSize, transferSize - offset);
        final chunk = Uint8List(length);
        for (var index = 0; index < length; index++) {
          chunk[index] = (offset + index) % 251;
        }
        await session.input.write(chunk);
        offset += length;
      }

      final exit = await session.done.timeout(const Duration(seconds: 30));
      await outputDone.future;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(mismatched, isFalse);
      expect(received, transferSize);
    },
    skip: skipReason,
  );

  test(
    'reports a typed error for a missing executable',
    () async {
      await expectLater(
        Pty.spawn(
          const PtySpawnOptions(executable: '/path/that/does/not/exist'),
        ),
        throwsA(
          isA<PtySpawnException>().having(
            (exception) => exception.nativeError?.kind,
            'native error kind',
            PtyErrorKind.notFound,
          ),
        ),
      );
    },
    skip: nativeSkipReason,
  );

  test(
    'reports a typed error for an invalid working directory',
    () async {
      await expectLater(
        Pty.spawn(
          const PtySpawnOptions(
            executable: '/bin/sh',
            workingDirectory: '/path/that/does/not/exist',
          ),
        ),
        throwsA(
          isA<PtySpawnException>().having(
            (exception) => exception.nativeError?.kind,
            'native error kind',
            PtyErrorKind.workingDirectoryFailed,
          ),
        ),
      );
    },
    skip: nativeSkipReason,
  );

  test(
    'resizes, kills, and closes a live session idempotently',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/bin/sh',
          arguments: ['-c', 'sleep 30'],
        ),
      );
      session.resize(const PtySize(columns: 120, rows: 40));
      session.kill();

      final exit = await session.processExit.timeout(
        const Duration(seconds: 5),
        onTimeout: () => throw StateError('killed process did not exit'),
      );
      expect(exit, isA<PtySignalExit>());
      if (exit case PtySignalExit(:final signal)) expect(signal, 9);

      await Future.wait([session.close(), session.close()]);
    },
    skip: nativeSkipReason,
  );

  test(
    'fails a pending write when the session closes',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['slow-copy-input', '4194304', '10'],
          inputBufferBytes: 64 * 1024,
        ),
      );
      final outputSubscription = session.output.listen((_) {});
      final writeFuture = session.input.write(Uint8List(4 * 1024 * 1024));
      final writeExpectation = expectLater(
        writeFuture,
        throwsA(isA<PtyException>()),
      );
      await Future<void>.delayed(const Duration(milliseconds: 50));
      await session.close();
      await outputSubscription.cancel();

      await writeExpectation;
    },
    skip: skipReason,
  );
}

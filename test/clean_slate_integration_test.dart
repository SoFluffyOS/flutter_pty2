import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final fixture = Platform.environment['PTY_TEST_CHILD'];
  final nativeConfigured =
      (Platform.isLinux || Platform.isMacOS) && library?.isNotEmpty == true;
  final integrationConfigured = (Platform.isLinux || Platform.isMacOS) &&
      library?.isNotEmpty == true &&
      fixture?.isNotEmpty == true;
  final nativeSkipReason = switch (nativeConfigured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY on Linux or macOS to run native '
        'integration tests.',
  };
  final skipReason = switch (integrationConfigured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY and PTY_TEST_CHILD on Linux or macOS '
        'to run native integration tests.',
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
    'reports Unix PTY capabilities accurately',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/bin/sh',
          arguments: ['-c', 'exit 0'],
        ),
      );
      final exit = await session.done;
      await session.close();
      final capabilities = session.capabilities;

      expect(exit, isA<PtyExitCode>());
      expect(capabilities.posixSignals, isTrue);
      expect(capabilities.foregroundProcessGroups, isTrue);
      expect(capabilities.pixelDimensions, isTrue);
      expect(capabilities.reliableProcessTreeKill, isFalse);
      expect(capabilities.conPty, isFalse);
    },
    skip: nativeSkipReason,
  );

  test(
    'kills a same-process-group child before done completes',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['spawn-child'],
        ),
      );
      final started = Completer<void>();
      final outputDone = Completer<void>();
      var text = '';
      final subscription = session.output.listen(
        (chunk) {
          text += utf8.decode(chunk, allowMalformed: true);
          if (text.contains('child-started')) {
            if (started.isCompleted) return;
            started.complete();
          }
        },
        onDone: outputDone.complete,
      );
      try {
        await started.future.timeout(const Duration(seconds: 5));
        session.kill();
        final exit = await session.done.timeout(const Duration(seconds: 5));
        await outputDone.future.timeout(const Duration(seconds: 5));

        expect(exit, isA<PtySignalExit>());
        expect(text, contains('child-started'));
      } finally {
        await subscription.cancel();
        await session.close();
      }
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
    'kills a same-process-group grandchild before done completes',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['spawn-grandchild'],
        ),
      );
      final started = Completer<void>();
      final outputDone = Completer<void>();
      var text = '';
      final subscription = session.output.listen(
        (chunk) {
          text += utf8.decode(chunk, allowMalformed: true);
          if (text.contains('child-started')) {
            if (started.isCompleted) return;
            started.complete();
          }
        },
        onDone: outputDone.complete,
      );
      try {
        await started.future.timeout(const Duration(seconds: 5));
        session.kill();
        final exit = await session.done.timeout(const Duration(seconds: 5));
        await outputDone.future.timeout(const Duration(seconds: 5));

        expect(exit, isA<PtySignalExit>());
        expect(text, contains('child-started'));
      } finally {
        await subscription.cancel();
        await session.close();
      }
    },
    skip: skipReason,
  );

  test(
    'preserves empty, quoted, spaced, and Unicode arguments',
    () async {
      final child = fixture;
      if (child == null) return;
      const arguments = [
        'empty',
        '',
        'hello world',
        'quote"backslash\\',
        r'C:\Program Files\Test\',
        '你好🙂',
      ];
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: ['print-argv', ...arguments],
        ),
      );
      final output = await session.output.toList();
      final exit = await session.done;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(
        output.expand((chunk) => chunk).toList(),
        utf8.encode([
          for (var index = 0; index < arguments.length; index++)
            '$index:${arguments[index]}\n',
        ].join()),
      );
    },
    skip: skipReason,
  );

  test(
    'passes Unicode and empty environment values to the child',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['print-env'],
          environment: const PtyEnvironment.replace({
            'PTY_TEST_VALUE': '你好🙂',
            'PTY_EMPTY_VALUE': '',
          }),
        ),
      );
      final output = await session.output.toList();
      final exit = await session.done;
      await session.close();
      final text = utf8.decode(output.expand((chunk) => chunk).toList());

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(text.split('\n'), contains('PTY_TEST_VALUE=你好🙂'));
      expect(text.split('\n'), contains('PTY_EMPTY_VALUE='));
    },
    skip: skipReason,
  );

  test(
    'starts the child in the requested working directory',
    () async {
      final child = fixture;
      if (child == null) return;
      final expectedWorkingDirectory =
          await Directory('/tmp').resolveSymbolicLinks();
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['print-cwd'],
          workingDirectory: '/tmp',
        ),
      );
      final output = await session.output.toList();
      final exit = await session.done;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(
        utf8.decode(output.expand((chunk) => chunk).toList()),
        '$expectedWorkingDirectory\n',
      );
    },
    skip: skipReason,
  );

  test(
    'starts the child in a Unicode working directory',
    () async {
      final child = fixture;
      if (child == null) return;
      final directory = await Directory.systemTemp.createTemp('pty-cwd-你好-');
      try {
        final expectedWorkingDirectory = await directory.resolveSymbolicLinks();
        final session = await Pty.spawn(
          PtySpawnOptions(
            executable: child,
            arguments: const ['print-cwd'],
            workingDirectory: directory.path,
          ),
        );
        final output = await session.output.toList();
        final exit = await session.done;
        await session.close();

        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        expect(
          utf8.decode(output.expand((chunk) => chunk).toList()),
          '$expectedWorkingDirectory\n',
        );
      } finally {
        await directory.delete();
      }
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
    'preserves invalid UTF-8 and embedded NUL bytes in tiny writes',
    () async {
      final child = fixture;
      if (child == null) return;
      final expected = Uint8List.fromList([0, 1, 127, 128, 191, 192, 255, 0]);
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: ['copy-input', '${expected.length}'],
        ),
      );
      final outputFuture = session.output.toList();
      for (final byte in expected) {
        await session.input.write(Uint8List.fromList([byte]));
      }
      final exit = await session.done;
      final output = await outputFuture;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(output.expand((chunk) => chunk).toList(), expected);
    },
    skip: skipReason,
  );

  test(
    'flushes an accepted tryWrite before process completion',
    () async {
      final child = fixture;
      if (child == null) return;
      const transferSize = 64 * 1024;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['copy-input', '$transferSize'],
          inputBufferBytes: 64 * 1024,
        ),
      );
      var received = 0;
      var mismatched = false;
      final outputSubscription = session.output.listen((chunk) {
        received += chunk.length;
        if (chunk.any((byte) => byte != 0)) mismatched = true;
      });
      expect(
        session.input.tryWrite(Uint8List(transferSize)),
        PtyWriteResult.accepted,
      );
      await session.input.flush();
      final exit = await session.done;
      await outputSubscription.cancel();

      expect(received, transferSize);
      expect(mismatched, isFalse);
      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
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
    'closes cleanly while process exit and output drain are racing',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/bin/sh',
          arguments: [
            '-c',
            'printf close-race-sentinel; sleep 30; exit 23',
          ],
        ),
      );
      final outputFuture = session.output.toList();
      final closeFuture = session.close();
      final processExit = await session.processExit;
      final done = await session.done;
      await closeFuture;
      await outputFuture;

      expect(processExit, isA<PtySignalExit>());
      if (processExit case PtySignalExit(:final signal)) expect(signal, 9);
      expect(done, processExit);
    },
    skip: nativeSkipReason,
  );

  test(
    'rejects native operations after close',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/bin/sh',
          arguments: ['-c', 'sleep 30'],
        ),
      );
      await session.close();

      expect(() => session.pid, throwsA(isA<PtyClosedException>()));
      expect(
        () => session.resize(const PtySize(columns: 120, rows: 40)),
        throwsA(isA<PtyClosedException>()),
      );
      expect(() => session.kill(), throwsA(isA<PtyClosedException>()));
      expect(
        () => session.sendSignal(PosixSignal.term),
        throwsA(isA<PtyClosedException>()),
      );
      expect(
        session.input.tryWrite(Uint8List.fromList([1])),
        PtyWriteResult.closed,
      );
    },
    skip: nativeSkipReason,
  );

  test(
    'close discards output buffered before the first listener',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/bin/sh',
          arguments: [
            '-c',
            'printf buffered-close-output; sleep 30',
          ],
        ),
      );
      await Future<void>.delayed(const Duration(milliseconds: 100));

      await session.close();
      final output = await session.output.toList().timeout(
            const Duration(seconds: 5),
          );

      expect(output, isEmpty);
    },
    skip: nativeSkipReason,
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
    'reports a typed error for an executable without execute permission',
    () async {
      await expectLater(
        Pty.spawn(const PtySpawnOptions(executable: '/')),
        throwsA(
          isA<PtySpawnException>().having(
            (exception) => exception.nativeError?.kind,
            'native error kind',
            PtyErrorKind.permissionDenied,
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
    'resizes repeatedly while output is active',
    () async {
      final child = fixture;
      if (child == null) return;
      const transferSize = 1000000;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['flood-output', '$transferSize'],
        ),
      );
      final outputFuture = session.output.toList();
      const sizes = [
        PtySize(columns: 80, rows: 24),
        PtySize(columns: 120, rows: 40),
        PtySize(columns: 200, rows: 60),
        PtySize(columns: 50, rows: 10),
      ];
      for (var index = 0; index < 20; index++) {
        session.resize(sizes[index % sizes.length]);
      }
      final exit = await session.done.timeout(const Duration(seconds: 10));
      final output = await outputFuture;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(
        output.expand((chunk) => chunk).toList(),
        List<int>.generate(transferSize, (index) => index % 251),
      );
    },
    skip: skipReason,
  );

  test(
    'delivers resized dimensions to the child PTY',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['print-size-after', '100'],
          size: const PtySize(
            columns: 80,
            rows: 24,
            pixelWidth: 640,
            pixelHeight: 480,
          ),
        ),
      );
      final outputFuture = session.output.toList();
      session.resize(
        const PtySize(
          columns: 120,
          rows: 40,
          pixelWidth: 1920,
          pixelHeight: 1080,
        ),
      );
      final exit = await session.done;
      final output = await outputFuture;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(
        output.expand((chunk) => chunk).toList(),
        '40 120 1920 1080\n'.codeUnits,
      );
    },
    skip: skipReason,
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

  test(
    'sends a POSIX signal to the foreground process group',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/bin/sh',
          arguments: ['-c', 'sleep 30'],
        ),
      );
      session.sendSignal(PosixSignal.term);

      final exit = await session.processExit.timeout(
        const Duration(seconds: 5),
        onTimeout: () => throw StateError('signaled process did not exit'),
      );
      expect(exit, isA<PtySignalExit>());
      if (exit case PtySignalExit(:final signal)) expect(signal, 15);
      await session.close();
    },
    skip: nativeSkipReason,
  );

  test(
    'reports a crashed child as a signal exit',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(executable: child, arguments: const ['crash']),
      );
      final processExit = await session.processExit;
      final done = await session.done;
      await session.close();

      expect(processExit, isA<PtySignalExit>());
      if (processExit case PtySignalExit(:final signal)) expect(signal, 11);
      expect(done, processExit);
    },
    skip: skipReason,
  );
}

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty.dart' as legacy;
import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

String decodeTerminalText(Iterable<int> bytes) {
  return utf8.decode(bytes.toList()).replaceAll('\r\n', '\n');
}

const _binaryReadyMarker = 'BINARY_READY';

final class _BinaryOutput {
  _BinaryOutput(this.bytes, this.subscription);

  final List<int> bytes;
  final StreamSubscription<Uint8List> subscription;
}

Future<_BinaryOutput> _listenForBinaryOutput(PtySession session) async {
  final marker = _binaryReadyMarker.codeUnits;
  final bytes = <int>[];
  final ready = Completer<void>();
  late final StreamSubscription<Uint8List> subscription;
  subscription = session.output.listen((chunk) {
    bytes.addAll(chunk);
    if (ready.isCompleted || bytes.length < marker.length) return;
    for (var index = 0; index < marker.length; index++) {
      if (bytes[index] == marker[index]) continue;
      ready.completeError(
        StateError('binary fixture readiness marker was corrupted'),
      );
      return;
    }
    bytes.removeRange(0, marker.length);
    ready.complete();
  });
  try {
    await ready.future.timeout(const Duration(seconds: 5));
  } catch (_) {
    await subscription.cancel();
    rethrow;
  }
  return _BinaryOutput(bytes, subscription);
}

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
    'legacy Pty.start resolves the configured native library',
    () async {
      final pty = legacy.Pty.start(
        '/bin/sh',
        arguments: const ['-c', 'printf legacy-compat; exit 17'],
      );
      final outputFuture = pty.output.toList();
      try {
        final results = await Future.wait<Object?>([
          pty.exitCode,
          outputFuture,
        ]);
        expect(results[0], 17);
        expect(
          (results[1] as List<Uint8List>).expand((chunk) => chunk).toList(),
          'legacy-compat'.codeUnits,
        );
      } finally {
        pty.destroy();
      }
    },
    skip: nativeSkipReason,
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
        decodeTerminalText(output.expand((chunk) => chunk)),
        [
          for (var index = 0; index < arguments.length; index++)
            '$index:${arguments[index]}\n',
        ].join(),
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
      final text = decodeTerminalText(output.expand((chunk) => chunk));

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(text.split('\n'), contains('PTY_TEST_VALUE=你好🙂'));
      expect(text.split('\n'), contains('PTY_EMPTY_VALUE='));
    },
    skip: skipReason,
  );

  test(
    'inherits, overrides, and removes environment values',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['print-env'],
          environment: const PtyEnvironment.inherit(
            overrides: {'PTY_TEST_VALUE': '继承🙂'},
            remove: {'PATH'},
          ),
        ),
      );
      final output = await session.output.toList();
      final exit = await session.done;
      await session.close();
      final lines = decodeTerminalText(output.expand((chunk) => chunk)).split(
        '\n',
      );

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(lines, contains('PTY_TEST_VALUE=继承🙂'));
      expect(
        lines.where((line) => line.toUpperCase().startsWith('PATH=')),
        isEmpty,
      );
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
        decodeTerminalText(output.expand((chunk) => chunk)),
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
          decodeTerminalText(output.expand((chunk) => chunk)),
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
    'delivers Ctrl-C through Unix PTY line discipline',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['wait-for-sigint'],
        ),
      );
      final ready = Completer<void>();
      final outputDone = Completer<void>();
      var output = '';
      final subscription = session.output.listen(
        (chunk) {
          output += utf8.decode(chunk, allowMalformed: true);
          if (output.contains('READY') && !ready.isCompleted) {
            ready.complete();
          }
        },
        onDone: outputDone.complete,
      );
      try {
        await ready.future.timeout(const Duration(seconds: 5));
        await session.input.write(Uint8List.fromList(const [0x03]));
        final exit = await session.done.timeout(const Duration(seconds: 5));
        await outputDone.future.timeout(const Duration(seconds: 5));

        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        expect(output, contains('SIGINT'));
      } finally {
        await subscription.cancel();
        await session.close();
      }
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
      final binaryOutput = await _listenForBinaryOutput(session);
      try {
        for (final byte in expected) {
          await session.input.write(Uint8List.fromList([byte]));
        }
        final exit = await session.done;

        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        expect(binaryOutput.bytes, expected);
      } finally {
        await binaryOutput.subscription.cancel();
        await session.close();
      }
    },
    skip: skipReason,
  );

  test(
    'round-trips required binary transfer sizes without byte changes',
    () async {
      final child = fixture;
      if (child == null) return;
      const transferSizes = [
        1,
        16,
        4 * 1024,
        64 * 1024,
        1024 * 1024,
        10 * 1024 * 1024,
      ];

      for (final transferSize in transferSizes) {
        final expected = Uint8List.fromList([
          for (var index = 0; index < transferSize; index++) index & 0xff,
        ]);
        final session = await Pty.spawn(
          PtySpawnOptions(
            executable: child,
            arguments: ['slow-copy-input', '$transferSize', '1'],
          ),
        );
        try {
          final binaryOutput = await _listenForBinaryOutput(session);
          for (var offset = 0; offset < expected.length;) {
            final end = math.min(offset + 64 * 1024, expected.length);
            await session.input.write(expected.sublist(offset, end));
            offset = end;
          }
          final exit = await session.processExit.timeout(
            const Duration(seconds: 30),
          );

          expect(exit, isA<PtyExitCode>());
          if (exit case PtyExitCode(:final code)) expect(code, 0);
          expect(binaryOutput.bytes, expected);
          await binaryOutput.subscription.cancel();
        } finally {
          await session.close();
        }
      }
    },
    skip: skipReason,
    timeout: const Timeout(Duration(minutes: 2)),
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
      final binaryOutput = await _listenForBinaryOutput(session);
      try {
        expect(
          session.input.tryWrite(Uint8List(transferSize)),
          PtyWriteResult.accepted,
        );
        await session.input.flush();
        final exit = await session.done;

        expect(binaryOutput.bytes.length, transferSize);
        expect(binaryOutput.bytes.every((byte) => byte == 0), isTrue);
        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
      } finally {
        await binaryOutput.subscription.cancel();
        await session.close();
      }
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
    'does not complete done before paused output is delivered',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['exit-after-output', '0', 'paused-sentinel'],
        ),
      );
      final received = <int>[];
      final outputDone = Completer<void>();
      final subscription = session.output.listen(
        (chunk) => received.addAll(chunk),
        onDone: outputDone.complete,
      );
      subscription.pause();
      var paused = true;
      try {
        final processExit = await session.processExit.timeout(
          const Duration(seconds: 5),
        );
        expect(processExit, isA<PtyExitCode>());

        var doneObserved = false;
        final doneFuture = session.done.then((exit) {
          doneObserved = true;
          return exit;
        });
        await Future<void>.delayed(const Duration(milliseconds: 100));
        expect(doneObserved, isFalse);

        subscription.resume();
        paused = false;
        final done = await doneFuture.timeout(const Duration(seconds: 5));
        await outputDone.future.timeout(const Duration(seconds: 5));

        expect(done, processExit);
        expect(received, 'paused-sentinel'.codeUnits);
      } finally {
        if (paused) subscription.resume();
        await subscription.cancel();
        await session.close();
      }
    },
    skip: skipReason,
  );

  test(
    'drains PTY output buffered when the child closes',
    () async {
      final child = fixture;
      if (child == null) return;
      const transferSize = 16 * 1024 + 1;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['flood-output', '$transferSize'],
          outputWindowBytes: 16 * 1024,
        ),
      );
      final received = <int>[];
      final outputDone = Completer<void>();
      final subscription = session.output.listen(
        received.addAll,
        onDone: outputDone.complete,
      );
      subscription.pause();
      var paused = true;
      try {
        await Future<void>.delayed(const Duration(milliseconds: 100));
        subscription.resume();
        paused = false;
        final processExit = await session.processExit.timeout(
          const Duration(seconds: 5),
        );
        expect(processExit, isA<PtyExitCode>());
        final done = await session.done.timeout(const Duration(seconds: 5));
        await outputDone.future.timeout(const Duration(seconds: 5));

        expect(done, processExit);
        expect(
            received, List<int>.generate(transferSize, (index) => index % 251));
      } finally {
        if (paused) subscription.resume();
        await subscription.cancel();
        await session.close();
      }
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
    'allows output subscription cancellation after close',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/bin/sh',
          arguments: ['-c', 'sleep 30'],
        ),
      );
      final subscription = session.output.listen((_) {});

      await session.close();
      await subscription.cancel();
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
      final binaryOutput = await _listenForBinaryOutput(session);
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
        final exit = await session.done;

        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        expect(binaryOutput.bytes.length, transferSize);
        expect(
          binaryOutput.bytes,
          List<int>.generate(transferSize, (index) => index % 251),
        );
      } finally {
        await binaryOutput.subscription.cancel();
        await session.close();
      }
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
      final binaryOutput = await _listenForBinaryOutput(session);
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
        final exit = await session.done.timeout(const Duration(seconds: 30));

        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        expect(binaryOutput.bytes.length, transferSize);
        expect(
          binaryOutput.bytes,
          List<int>.generate(transferSize, (index) => index % 251),
        );
      } finally {
        await binaryOutput.subscription.cancel();
        await session.close();
      }
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

      final firstClose = session.close();
      final secondClose = session.close();
      expect(identical(firstClose, secondClose), isTrue);
      await Future.wait([firstClose, secondClose]);
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
        decodeTerminalText(output.expand((chunk) => chunk)),
        '40 120 1920 1080\n',
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
      final binaryOutput = await _listenForBinaryOutput(session);
      try {
        final writeFuture = session.input.write(Uint8List(4 * 1024 * 1024));
        final writeExpectation = expectLater(
          writeFuture,
          throwsA(isA<PtyException>()),
        );
        await Future<void>.delayed(const Duration(milliseconds: 50));
        await session.close();

        await writeExpectation;
      } finally {
        await binaryOutput.subscription.cancel();
      }
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

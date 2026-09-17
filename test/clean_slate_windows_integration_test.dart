import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final fixture = Platform.environment['PTY_TEST_CHILD'];
  final configured = Platform.isWindows &&
      library?.isNotEmpty == true &&
      fixture?.isNotEmpty == true;
  final skipReason = switch (configured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY and PTY_TEST_CHILD on Windows to run '
        'clean-slate integration tests.',
  };

  test(
    'preserves Windows argv quoting and Unicode arguments',
    () async {
      final child = fixture;
      if (child == null) return;
      const arguments = [
        'hello',
        '',
        'hello world',
        '"',
        r'\',
        'quote"backslash\\',
        r'C:\Program Files\Test\',
        r'abc\"def',
        '你好',
        '🙂',
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
        utf8.decode(output.expand((chunk) => chunk).toList()),
        [
          for (var index = 0; index < arguments.length; index++)
            '$index:${arguments[index]}\n',
        ].join(),
      );
    },
    skip: skipReason,
  );

  test(
    'passes Unicode and empty Windows environment values',
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
    'inherits, overrides, and removes Windows environment values',
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
      final lines = utf8.decode(output.expand((chunk) => chunk).toList()).split(
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
    'treats an empty Windows working directory as inherited',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['exit', '0'],
          workingDirectory: '',
        ),
      );
      final exit = await session.done;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
    },
    skip: skipReason,
  );

  test(
    'reports typed errors for Windows spawn failures',
    () async {
      final child = fixture;
      if (child == null) return;

      await expectLater(
        Pty.spawn(
          const PtySpawnOptions(
            executable: r'C:\flutter-pty-missing\pty_test_child.exe',
          ),
        ),
        throwsA(
          isA<PtySpawnException>().having(
            (exception) => exception.nativeError?.kind,
            'native error kind',
            PtyErrorKind.notFound,
          ),
        ),
      );

      await expectLater(
        Pty.spawn(
          PtySpawnOptions(
            executable: child,
            workingDirectory: r'C:\flutter-pty-missing-working-directory',
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
    skip: skipReason,
  );

  test(
    'round-trips binary input through ConPTY',
    () async {
      final child = fixture;
      if (child == null) return;
      final expected = Uint8List.fromList([
        for (var index = 0; index < 64 * 1024; index++) index % 251,
      ]);
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: ['copy-input', '${expected.length}'],
        ),
      );
      final outputFuture = session.output.toList();
      await session.input.write(expected);
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
    'preserves many tiny binary writes through ConPTY',
    () async {
      final child = fixture;
      if (child == null) return;
      final expected = Uint8List.fromList([
        for (var index = 0; index < 4096; index++) index % 251,
      ]);
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: ['copy-input', '${expected.length}'],
        ),
      );
      final outputFuture = session.output.toList();
      try {
        for (final byte in expected) {
          await session.input.write(Uint8List.fromList([byte]));
        }
        final exit = await session.done;
        final output = await outputFuture;

        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        expect(output.expand((chunk) => chunk).toList(), expected);
      } finally {
        await session.close();
      }
    },
    skip: skipReason,
  );

  test(
    'flushes an accepted tryWrite before process completion',
    () async {
      final child = fixture;
      if (child == null) return;
      final expected = Uint8List.fromList([
        for (var index = 0; index < 64 * 1024; index++) index % 251,
      ]);
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: ['copy-input', '${expected.length}'],
        ),
      );
      final outputFuture = session.output.toList();
      try {
        expect(
          session.input.tryWrite(expected),
          PtyWriteResult.accepted,
        );
        await session.input.flush();
        final exit = await session.done;
        final output = await outputFuture;

        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        expect(output.expand((chunk) => chunk).toList(), expected);
      } finally {
        await session.close();
      }
    },
    skip: skipReason,
  );

  test(
    'keeps trailing ConPTY output available after process exit until done',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['exit-after-output', '37', 'windows-sentinel'],
        ),
      );
      final outputFuture = session.output.toList();
      final processExit = await session.processExit;
      final done = await session.done;
      final output = await outputFuture;
      await session.close();

      expect(processExit, isA<PtyExitCode>());
      if (processExit case PtyExitCode(:final code)) expect(code, 37);
      expect(done, processExit);
      expect(
        utf8.decode(output.expand((chunk) => chunk).toList()),
        'windows-sentinel',
      );
    },
    skip: skipReason,
  );

  test(
    'closes cleanly while ConPTY input is queued',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['slow-input', '10'],
          inputBufferBytes: 64 * 1024,
        ),
      );
      final bytes = Uint8List(64 * 1024);
      final writes = <Future<void>>[
        for (var index = 0; index < 16; index++)
          session.input.write(bytes).catchError((_) {}),
      ];

      await Future<void>.delayed(const Duration(milliseconds: 10));
      await session.close().timeout(const Duration(seconds: 5));
      await Future.wait(writes);
    },
    skip: skipReason,
  );

  test(
    'pausing output resumes a bounded ConPTY flood',
    () async {
      final child = fixture;
      if (child == null) return;
      const transferSize = 1000000;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['flood-output', '$transferSize'],
          outputWindowBytes: 16 * 1024,
        ),
      );
      final chunks = <Uint8List>[];
      final subscription = session.output.listen(chunks.add);
      subscription.pause();
      await Future<void>.delayed(const Duration(milliseconds: 100));
      subscription.resume();
      final exit = await session.done.timeout(const Duration(seconds: 10));
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(
        chunks.expand((chunk) => chunk).toList(),
        List<int>.generate(transferSize, (index) => index % 251),
      );
    },
    skip: skipReason,
  );

  test(
    'reports Windows capabilities and rejects POSIX signals',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['slow-output', '1000000', '1'],
        ),
      );

      expect(
        () => session.sendSignal(PosixSignal.term),
        throwsA(isA<PtyUnsupportedException>()),
      );
      expect(session.capabilities.posixSignals, isFalse);
      expect(session.capabilities.foregroundProcessGroups, isFalse);
      expect(session.capabilities.pixelDimensions, isFalse);
      expect(session.capabilities.reliableProcessTreeKill, isTrue);
      expect(session.capabilities.conPty, isTrue);
      await session.close();
    },
    skip: skipReason,
  );

  test(
    'kills a Job Object child before done completes',
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
        await session.done.timeout(const Duration(seconds: 5));
        await outputDone.future.timeout(const Duration(seconds: 5));

        expect(text, contains('child-started'));
      } finally {
        await subscription.cancel();
        await session.close();
      }
    },
    skip: skipReason,
  );

  test(
    'keeps kill idempotent after the process exits',
    () async {
      final child = fixture;
      if (child == null) return;
      final session = await Pty.spawn(
        PtySpawnOptions(
          executable: child,
          arguments: const ['exit', '0'],
        ),
      );
      final output = session.output.toList();
      await session.processExit.timeout(const Duration(seconds: 5));
      session.kill();
      session.kill();
      await session.done.timeout(const Duration(seconds: 5));
      await output;
      await session.close();
    },
    skip: skipReason,
  );

  test(
    'resizes a ConPTY while reporting unsupported pixel dimensions',
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
      final output = session.output.toList();
      session.resize(
        const PtySize(
          columns: 120,
          rows: 40,
          pixelWidth: 1920,
          pixelHeight: 1080,
        ),
      );
      final exit = await session.done;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(await output, '0 0 0 0\n'.codeUnits);
    },
    skip: skipReason,
  );

  test(
    'kills a Job Object grandchild before done completes',
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
        await session.done.timeout(const Duration(seconds: 5));
        await outputDone.future.timeout(const Duration(seconds: 5));

        expect(text, contains('child-started'));
      } finally {
        await subscription.cancel();
        await session.close();
      }
    },
    skip: skipReason,
  );
}

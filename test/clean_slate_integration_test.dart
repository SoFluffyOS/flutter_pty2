import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final fixture = Platform.environment['PTY_TEST_CHILD'];
  final integrationConfigured = Platform.isMacOS &&
      library?.isNotEmpty == true &&
      fixture?.isNotEmpty == true;
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
}

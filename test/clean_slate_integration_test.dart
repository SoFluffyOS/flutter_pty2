import 'dart:io';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final skipReason = switch ((Platform.isMacOS, library)) {
    (true, final path?) when path.isNotEmpty => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY on macOS to run native integration tests.',
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
}

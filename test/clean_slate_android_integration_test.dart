import 'dart:io';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final skipReason = switch (Platform.isAndroid) {
    true => null,
    _ => 'Run this test on an Android emulator or device.',
  };

  test(
    'spawns Android shell output through the clean-slate API',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/system/bin/sh',
          arguments: ['-c', 'printf android-clean-slate; exit 17'],
        ),
      );
      final outputFuture = session.output.toList();
      final exit = await session.done.timeout(const Duration(seconds: 10));
      final output = await outputFuture;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 17);
      expect(output.expand((chunk) => chunk).toList(),
          'android-clean-slate'.codeUnits);
    },
    skip: skipReason,
  );

  test(
    'round-trips Android shell input bytes',
    () async {
      final session = await Pty.spawn(
        const PtySpawnOptions(
          executable: '/system/bin/sh',
          arguments: [
            '-c',
            'IFS= read line; printf android-response:%s "\$line"',
          ],
        ),
      );
      final outputFuture = session.output.toList();
      await session.input.writeUtf8('android-input\n');
      final exit = await session.done.timeout(const Duration(seconds: 10));
      final output = await outputFuture;
      await session.close();

      expect(exit, isA<PtyExitCode>());
      if (exit case PtyExitCode(:final code)) expect(code, 0);
      expect(
        output.expand((chunk) => chunk).toList(),
        containsAllInOrder('android-response:android-input'.codeUnits),
      );
    },
    skip: skipReason,
  );
}

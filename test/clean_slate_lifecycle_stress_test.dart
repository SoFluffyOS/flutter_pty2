import 'dart:io';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final nativeConfigured =
      (Platform.isLinux || Platform.isMacOS) && library?.isNotEmpty == true;
  final skipReason = switch (nativeConfigured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY on Linux or macOS to run lifecycle stress.',
  };

  test(
    'completes 1000 spawn, done, and close cycles',
    () async {
      for (var cycle = 0; cycle < 1000; cycle++) {
        final session = await Pty.spawn(
          const PtySpawnOptions(
            executable: '/bin/sh',
            arguments: ['-c', 'exit 0'],
          ),
        );
        final exit = await session.done.timeout(const Duration(seconds: 5));
        expect(exit, isA<PtyExitCode>());
        if (exit case PtyExitCode(:final code)) expect(code, 0);
        await session.close();
      }
    },
    skip: skipReason,
    timeout: const Timeout(Duration(minutes: 10)),
  );
}

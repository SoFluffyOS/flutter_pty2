import 'dart:io';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final fixture = Platform.environment['PTY_TEST_CHILD'];
  final nativeConfigured = switch (Platform.isWindows) {
    true => library?.isNotEmpty == true && fixture?.isNotEmpty == true,
    false =>
      (Platform.isLinux || Platform.isMacOS) && library?.isNotEmpty == true,
  };
  final skipReason = switch (nativeConfigured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY and PTY_TEST_CHILD on a desktop '
        'platform to run lifecycle stress.',
  };

  test(
    'completes 1000 spawn, done, and close cycles',
    () async {
      final child = switch (Platform.isWindows) {
        true => fixture,
        false => '/bin/sh',
      };
      if (child == null) return;
      final arguments = switch (Platform.isWindows) {
        true => const ['exit', '0'],
        false => const ['-c', 'exit 0'],
      };
      for (var cycle = 0; cycle < 1000; cycle++) {
        final session = await Pty.spawn(
          PtySpawnOptions(
            executable: child,
            arguments: arguments,
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

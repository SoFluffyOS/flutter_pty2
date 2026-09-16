import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('spawns Android shell output through the clean-slate API', (
    _,
  ) async {
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
    if (exit case PtyExitCode(:final code)) {
      expect(code, 17);
    }
    expect(
      output.expand((chunk) => chunk).toList(),
      'android-clean-slate'.codeUnits,
    );
  });

  testWidgets('round-trips Android shell input bytes', (_) async {
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
    await session.input.write(Uint8List.fromList('android-input\n'.codeUnits));
    final exit = await session.done.timeout(const Duration(seconds: 10));
    final output = await outputFuture;
    await session.close();

    expect(exit, isA<PtyExitCode>());
    if (exit case PtyExitCode(:final code)) {
      expect(code, 0);
    }
    expect(
      output.expand((chunk) => chunk).toList(),
      containsAllInOrder('android-response:android-input'.codeUnits),
    );
  });
}

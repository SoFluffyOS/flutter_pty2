import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('spawns an iOS shell and drains raw output', (_) async {
    final session = await Pty.spawn(
      const PtySpawnOptions(
        executable: '/bin/sh',
        arguments: ['-c', 'printf ios-clean-slate; exit 19'],
      ),
    );
    final outputFuture = session.output.toList();
    final exit = await session.done.timeout(const Duration(seconds: 10));
    final output = await outputFuture;
    await session.close();

    expect(exit, isA<PtyExitCode>());
    if (exit case PtyExitCode(:final code)) {
      expect(code, 19);
    }
    expect(
      output.expand((chunk) => chunk).toList(),
      Uint8List.fromList('ios-clean-slate'.codeUnits),
    );
  });

  testWidgets('round-trips iOS shell input bytes', (_) async {
    final session = await Pty.spawn(
      const PtySpawnOptions(
        executable: '/bin/sh',
        arguments: ['-c', 'IFS= read line; printf ios-response:%s "\$line"'],
      ),
    );
    final outputFuture = session.output.toList();
    await session.input.write(Uint8List.fromList('ios-input\n'.codeUnits));
    final exit = await session.done.timeout(const Duration(seconds: 10));
    final output = await outputFuture;
    await session.close();

    expect(exit, isA<PtyExitCode>());
    if (exit case PtyExitCode(:final code)) {
      expect(code, 0);
    }
    expect(
      output.expand((chunk) => chunk).toList(),
      containsAllInOrder('ios-response:ios-input'.codeUnits),
    );
  });
}

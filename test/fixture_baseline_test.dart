import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

void main() {
  final fixturePath = Platform.environment['PTY_TEST_CHILD'];
  final hasFixture = fixturePath != null && fixturePath.isNotEmpty;

  test(
    'fixture preserves arguments and output before exit',
    () async {
      final fixture = fixturePath;
      if (fixture == null || fixture.isEmpty) return;

      final process = await Process.start(
        fixture,
        const ['exit-after-output', '37', 'fixture-sentinel'],
        runInShell: false,
      );

      final outputFuture = process.stdout.fold<List<int>>(
        <int>[],
        (output, chunk) => output..addAll(chunk),
      );
      final errorFuture = process.stderr.drain<void>();
      final results = await Future.wait<Object?>([
        outputFuture,
        errorFuture,
        process.exitCode,
      ]);

      expect(results[0], Uint8List.fromList('fixture-sentinel'.codeUnits));
      expect(results[2], 37);
    },
    skip: switch (hasFixture) {
      true => null,
      _ => 'Set PTY_TEST_CHILD to run fixture tests.',
    },
  );
}

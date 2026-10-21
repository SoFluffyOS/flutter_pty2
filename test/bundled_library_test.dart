@TestOn('mac-os || linux')
library;

import 'dart:convert';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:test/test.dart';

/// Runs without `FLUTTER_PTY2_LIBRARY`, so the session uses the library that
/// `hook/build.dart` builds and bundles.
void main() {
  test('runs a shell with the library built by the build hook', () async {
    final session = await Pty.spawn(
      const PtySpawnOptions(
        executable: '/bin/sh',
        arguments: ['-c', 'printf "bundled:%s" "\$((6 * 7))"'],
        size: PtySize(columns: 80, rows: 24),
      ),
    );
    addTearDown(session.close);

    final output = session.output
        .cast<List<int>>()
        .transform(utf8.decoder)
        .join();

    expect(
      await session.done,
      isA<PtyExitCode>().having((exit) => exit.code, 'code', 0),
    );
    expect(await output, contains('bundled:42'));
  });
}

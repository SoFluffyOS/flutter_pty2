import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_pty2/src/internal/internal.dart';

Future<void> main() async {
  writeBenchmarkHeader();
  for (final size in benchmarkSizes) {
    final payload = benchmarkPattern(size);
    final summary = await runBenchmark('input', () async {
      final session = await Pty.spawn(
        benchmarkFixtureOptions(['copy-input', '$size']),
      );
      try {
        final outputFuture = collectBenchmarkOutput(session);
        final stopwatch = Stopwatch()..start();
        await session.input.write(payload);
        final exit = await session.done;
        final output = await outputFuture;
        stopwatch.stop();

        _expectSuccessfulTransfer(exit, output, payload);
        return stopwatch.elapsed;
      } finally {
        await session.close();
      }
    });
    writeBenchmark(summary, bytes: size);
  }
}

void _expectSuccessfulTransfer(
  PtyExit exit,
  Uint8List output,
  Uint8List expected,
) {
  if (exit case PtyExitCode(:final code) when code != 0) {
    throw StateError('Input benchmark child exited with code $code.');
  }
  if (output.length != expected.length) {
    throw StateError(
      'Input benchmark received ${output.length} bytes, '
      'expected ${expected.length}.',
    );
  }
  for (var index = 0; index < output.length; index++) {
    if (output[index] != expected[index]) {
      throw StateError('Input benchmark output mismatch at byte $index.');
    }
  }
}

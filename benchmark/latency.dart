import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_pty2/src/internal/internal.dart';

Future<void> main() async {
  writeBenchmarkHeader();
  final firstOutput = await runBenchmark('first_output_latency', () async {
    final session = await Pty.spawn(
      benchmarkFixtureOptions(const ['slow-output', '1', '0']),
    );
    final firstByte = Completer<Duration>();
    final stopwatch = Stopwatch();
    final subscription = session.output.listen((chunk) {
      if (chunk.isEmpty || firstByte.isCompleted) return;
      stopwatch.stop();
      firstByte.complete(stopwatch.elapsed);
    }, onDone: () {
      if (firstByte.isCompleted) return;
      firstByte.completeError(StateError('Child produced no output.'));
    });
    try {
      stopwatch.start();
      await session.done;
      return await firstByte.future.timeout(const Duration(seconds: 5));
    } finally {
      await subscription.cancel();
      await session.close();
    }
  });
  writeBenchmark(firstOutput, bytes: 1);

  final roundTrip = await runBenchmark('one_byte_rtt', () async {
    final session = await Pty.spawn(
      benchmarkFixtureOptions(const ['copy-input', '1']),
    );
    final firstByte = Completer<void>();
    final subscription = session.output.listen((chunk) {
      if (chunk.isNotEmpty && !firstByte.isCompleted) {
        firstByte.complete();
      }
    }, onDone: () {
      if (firstByte.isCompleted) return;
      firstByte.completeError(StateError('Child produced no output.'));
    });
    try {
      final stopwatch = Stopwatch()..start();
      await session.input.write(Uint8List.fromList([0xa5]));
      await firstByte.future.timeout(const Duration(seconds: 5));
      await session.done;
      stopwatch.stop();
      return stopwatch.elapsed;
    } finally {
      await subscription.cancel();
      await session.close();
    }
  });
  writeBenchmark(roundTrip, bytes: 1);
}

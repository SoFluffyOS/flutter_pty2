import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_pty2/src/internal/internal.dart';

Future<void> main() async {
  writeBenchmarkHeader();
  final summary = await runBenchmark('close', () async {
    final session = await Pty.spawn(
      benchmarkFixtureOptions(const ['hold']),
    );
    final stopwatch = Stopwatch()..start();
    await session.close();
    stopwatch.stop();
    return stopwatch.elapsed;
  });
  writeBenchmark(summary);
}

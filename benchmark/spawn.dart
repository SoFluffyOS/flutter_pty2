import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_pty2/src/internal/internal.dart';

Future<void> main() async {
  writeBenchmarkHeader();
  final summary = await runBenchmark('spawn', () async {
    final stopwatch = Stopwatch()..start();
    final session = await Pty.spawn(
      benchmarkFixtureOptions(const ['exit', '0']),
    );
    stopwatch.stop();
    await session.close();
    return stopwatch.elapsed;
  });
  writeBenchmark(summary);
}

import 'dart:io';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_pty2/src/internal/internal.dart';

const _sessionCounts = [1, 10, 50, 100];

Future<void> main() async {
  writeBenchmarkHeader();
  for (final count in _sessionCounts) {
    final summary = await runBenchmark('concurrency', () async {
      final sessions = <PtySession>[];
      final stopwatch = Stopwatch()..start();
      try {
        await Future.wait(
          List<Future<void>>.generate(count, (index) async {
            final session = await Pty.spawn(
              benchmarkFixtureOptions(const ['echo', 'benchmark']),
            );
            sessions.add(session);
          }),
        );
        await Future.wait<Object?>([
          ...sessions.map((session) => session.done),
          ...sessions.map(collectBenchmarkOutput),
        ]);
        stopwatch.stop();
        return stopwatch.elapsed;
      } finally {
        await Future.wait(sessions.map((session) => session.close()));
      }
    });
    writeBenchmark(summary, sessions: count);
    stdout.writeln(
      'concurrency_rss_bytes,sessions=$count,rss=${ProcessInfo.currentRss}',
    );
  }
}

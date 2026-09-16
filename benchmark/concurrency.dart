import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_pty2/src/internal/internal.dart';

const _loadedOutputBytes = 64 * 1024;

Future<void> main() async {
  writeBenchmarkHeader();
  for (final count in benchmarkSessionCounts()) {
    final idle = await runBenchmark(
      'idle_concurrency',
      () => _runIdleCohort(count),
    );
    writeBenchmark(idle, sessions: count);

    final loaded = await runBenchmark(
      'loaded_concurrency',
      () => _runLoadedCohort(count),
    );
    writeBenchmark(loaded, bytes: _loadedOutputBytes, sessions: count);

    await _reportLiveMemory(count);
  }
}

Future<Duration> _runIdleCohort(int count) async {
  final sessions = <PtySession>[];
  final stopwatch = Stopwatch()..start();
  try {
    await Future.wait(
      List<Future<void>>.generate(count, (index) async {
        sessions.add(
          await Pty.spawn(benchmarkFixtureOptions(const ['hold'])),
        );
      }),
    );
    stopwatch.stop();
    return stopwatch.elapsed;
  } catch (error, stackTrace) {
    throw StateError(
      'Idle concurrency failed for $count sessions: '
      '${describeBenchmarkError(error)}\n$stackTrace',
    );
  } finally {
    await Future.wait(sessions.map((session) => session.close()));
  }
}

Future<Duration> _runLoadedCohort(int count) async {
  final sessions = <PtySession>[];
  final outputs = <Future<Uint8List>>[];
  final stopwatch = Stopwatch()..start();
  try {
    await Future.wait(
      List<Future<void>>.generate(count, (index) async {
        final session = await Pty.spawn(
          benchmarkFixtureOptions(
            ['flood-output', '$_loadedOutputBytes'],
          ),
        );
        sessions.add(session);
        outputs.add(collectBenchmarkOutput(session));
      }),
    );
    await Future.wait<Object?>([
      ...sessions.map((session) => session.done),
      ...outputs,
    ]);
    final results = await Future.wait(outputs);
    if (results.any((output) => output.length != _loadedOutputBytes)) {
      throw StateError('Loaded concurrency output size mismatch.');
    }
    stopwatch.stop();
    return stopwatch.elapsed;
  } catch (error, stackTrace) {
    throw StateError(
      'Loaded concurrency failed for $count sessions: '
      '${describeBenchmarkError(error)}\n$stackTrace',
    );
  } finally {
    await Future.wait(sessions.map((session) => session.close()));
  }
}

Future<void> _reportLiveMemory(int count) async {
  final sessions = <PtySession>[];
  final before = ProcessInfo.currentRss;
  try {
    await Future.wait(
      List<Future<void>>.generate(count, (index) async {
        sessions.add(
          await Pty.spawn(benchmarkFixtureOptions(const ['hold'])),
        );
      }),
    );
    final after = ProcessInfo.currentRss;
    final rssDelta = switch (after - before) {
      final delta when delta > 0 => delta,
      _ => 0,
    };
    stdout.writeln(
      'memory_per_session_bytes,sessions=$count,rss_delta=$rssDelta,'
      'per_session=${rssDelta ~/ count}',
    );
  } catch (error, stackTrace) {
    throw StateError(
      'Live memory measurement failed for $count sessions: '
      '${describeBenchmarkError(error)}\n$stackTrace',
    );
  } finally {
    await Future.wait(sessions.map((session) => session.close()));
  }
}

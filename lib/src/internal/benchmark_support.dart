import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';

const benchmarkSizes = <int>[
  1,
  16,
  64,
  1024,
  4096,
  16384,
  65536,
  1024 * 1024,
];

int benchmarkIterations() {
  return benchmarkEnvironmentInt('PTY_BENCHMARK_ITERATIONS', fallback: 5);
}

int benchmarkWarmups() {
  return benchmarkEnvironmentInt('PTY_BENCHMARK_WARMUPS', fallback: 1);
}

int benchmarkEnvironmentInt(String name, {required int fallback}) {
  final rawValue = Platform.environment[name];
  if (rawValue == null || rawValue.isEmpty) return fallback;

  final value = int.tryParse(rawValue);
  if (value == null || value < 1) {
    throw ArgumentError.value(
      rawValue,
      name,
      'must be a positive integer',
    );
  }
  return value;
}

String benchmarkEnvironment(String name) {
  final value = Platform.environment[name];
  if (value == null || value.isEmpty) {
    throw StateError('Set $name before running the PTY benchmark.');
  }
  return value;
}

PtySpawnOptions benchmarkFixtureOptions(
  List<String> arguments, {
  int inputBufferBytes = 1024 * 1024,
  int outputWindowBytes = 256 * 1024,
}) {
  return PtySpawnOptions(
    executable: benchmarkEnvironment('PTY_TEST_CHILD'),
    arguments: List<String>.unmodifiable(arguments),
    inputBufferBytes: inputBufferBytes,
    outputWindowBytes: outputWindowBytes,
  );
}

Uint8List benchmarkPattern(int length) {
  final data = Uint8List(length);
  for (var index = 0; index < length; index++) {
    data[index] = index % 251;
  }
  return data;
}

Future<Uint8List> collectBenchmarkOutput(PtySession session) async {
  final bytes = await session.output.fold<List<int>>(
    <int>[],
    (collected, chunk) => collected..addAll(chunk),
  );
  return Uint8List.fromList(bytes);
}

Future<BenchmarkSummary> runBenchmark(
  String name,
  Future<Duration> Function() sample, {
  int? iterations,
}) async {
  final warmups = benchmarkWarmups();
  final count = iterations ?? benchmarkIterations();
  for (var index = 0; index < warmups; index++) {
    await sample();
  }

  final samples = <Duration>[];
  for (var index = 0; index < count; index++) {
    samples.add(await sample());
  }
  return BenchmarkSummary(name, samples);
}

void writeBenchmarkHeader() {
  stdout.writeln(
    'name,bytes,sessions,min_us,p50_us,p95_us,mean_us,mean_mib_per_sec',
  );
}

void writeBenchmark(
  BenchmarkSummary summary, {
  int? bytes,
  int? sessions,
}) {
  final columns = <String>[summary.name];
  switch (bytes) {
    case final value?:
      columns.add('$value');
    case null:
      columns.add('');
  }
  switch (sessions) {
    case final value?:
      columns.add('$value');
    case null:
      columns.add('');
  }
  columns.addAll([
    '${summary.minimum.inMicroseconds}',
    '${summary.percentile(50).inMicroseconds}',
    '${summary.percentile(95).inMicroseconds}',
    summary.mean.inMicroseconds.toStringAsFixed(1),
    switch (bytes) {
      final value? => meanMiBPerSecond(summary, value).toStringAsFixed(2),
      null => '',
    },
  ]);
  stdout.writeln(columns.join(','));
}

double meanMiBPerSecond(BenchmarkSummary summary, int bytes) {
  final microseconds = math.max(summary.mean.inMicroseconds, 1);
  return bytes * 1000000 / microseconds / (1024 * 1024);
}

final class BenchmarkSummary {
  BenchmarkSummary(this.name, List<Duration> samples)
      : samples = List<Duration>.unmodifiable(samples) {
    if (samples.isEmpty) {
      throw ArgumentError.value(samples, 'samples', 'must not be empty');
    }
  }

  final String name;
  final List<Duration> samples;

  Duration get minimum {
    return samples.reduce(
      (first, second) => switch (first <= second) {
        true => first,
        false => second,
      },
    );
  }

  Duration get mean {
    final total = samples.fold<int>(
      0,
      (microseconds, sample) => microseconds + sample.inMicroseconds,
    );
    return Duration(microseconds: total ~/ samples.length);
  }

  Duration percentile(int percentile) {
    if (percentile < 0 || percentile > 100) {
      throw ArgumentError.value(percentile, 'percentile');
    }
    final sorted = List<Duration>.of(samples)
      ..sort((first, second) => first.compareTo(second));
    final index = ((sorted.length - 1) * percentile / 100).round();
    return sorted[index];
  }
}

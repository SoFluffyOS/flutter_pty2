import 'package:flutter_pty2/src/internal/internal.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('reads current process CPU time on supported hosts', () {
    final cpu = readCurrentProcessCpuTime();
    if (cpu == null) {
      markTestSkipped('Current-process CPU time is unavailable on this host.');
      return;
    }
    expect(cpu.userMicroseconds, greaterThanOrEqualTo(0));
    expect(cpu.systemMicroseconds, greaterThanOrEqualTo(0));
  });

  test('reports normalized CPU utilization for a benchmark run', () {
    final summary = BenchmarkSummary('test', [
      const Duration(microseconds: 10),
    ]);
    const usage = BenchmarkCpuUsage(
      userMicroseconds: 20,
      systemMicroseconds: 10,
      wallMicroseconds: 100,
    );
    final run = BenchmarkRun(summary, usage);

    expect(run.summary, same(summary));
    expect(run.cpuUsage, same(usage));
    expect(usage.totalMicroseconds, 30);
    expect(usage.utilizationPercent, greaterThan(0));
  });
}

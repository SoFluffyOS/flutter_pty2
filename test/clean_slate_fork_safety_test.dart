import 'dart:io';
import 'dart:isolate';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final configured =
      (Platform.isLinux || Platform.isMacOS) && library?.isNotEmpty == true;
  final skipReason = switch (configured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY on Linux or macOS to run fork-safety '
        'tests.',
  };

  test(
    'spawns PTYs while sibling isolates allocate continuously',
    () async {
      final allocators = <Isolate>[];
      final readyPorts = <ReceivePort>[];
      try {
        for (var index = 0; index < 2; index++) {
          final readyPort = ReceivePort();
          readyPorts.add(readyPort);
          allocators.add(
            await Isolate.spawn(_allocationWorker, readyPort.sendPort),
          );
          await readyPort.first.timeout(const Duration(seconds: 5));
        }

        for (var cycle = 0; cycle < 8; cycle++) {
          final session = await Pty.spawn(
            const PtySpawnOptions(
              executable: '/bin/sh',
              arguments: ['-c', 'exit 0'],
            ),
          );
          try {
            final exit = await session.done.timeout(
              const Duration(seconds: 10),
            );
            expect(exit, isA<PtyExitCode>());
            if (exit case PtyExitCode(:final code)) expect(code, 0);
          } finally {
            await session.close();
          }
        }
      } finally {
        for (final readyPort in readyPorts) {
          readyPort.close();
        }
        for (final allocator in allocators) {
          allocator.kill(priority: Isolate.immediate);
        }
      }
    },
    skip: skipReason,
    timeout: const Timeout(Duration(minutes: 2)),
  );
}

void _allocationWorker(SendPort readyPort) {
  readyPort.send(true);
  while (true) {
    List.generate(10000, (index) => '$index-allocation');
  }
}

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final fixture = Platform.environment['PTY_TEST_CHILD'];
  final configured = (Platform.isLinux || Platform.isMacOS) &&
      library?.isNotEmpty == true &&
      fixture?.isNotEmpty == true;
  final skipReason = switch (configured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY and PTY_TEST_CHILD on Linux or macOS '
        'to run exit-drain stress.',
  };

  test(
    'keeps trailing output ordered across repeated exit races',
    () async {
      final child = fixture;
      if (child == null) return;
      final configuredCycles = int.tryParse(
        Platform.environment['PTY_EXIT_DRAIN_CYCLES'] ?? '',
      );
      final cycles = switch (configuredCycles) {
        final value? when value > 0 => value,
        _ => 100,
      };

      for (var cycle = 0; cycle < cycles; cycle++) {
        final sentinel = 'exit-drain-$cycle';
        final session = await Pty.spawn(
          PtySpawnOptions(
            executable: child,
            arguments: ['exit-after-output', '0', sentinel],
            outputWindowBytes: 16 * 1024,
          ),
        );
        final received = <int>[];
        final outputDone = Completer<void>();
        final subscription = session.output.listen(
          received.addAll,
          onDone: outputDone.complete,
        );
        subscription.pause();
        var paused = true;
        try {
          final processExit = await session.processExit.timeout(
            const Duration(seconds: 5),
          );
          var doneObserved = false;
          final doneFuture = session.done.then((exit) {
            doneObserved = true;
            return exit;
          });
          await Future<void>.delayed(const Duration(milliseconds: 1));
          expect(
            doneObserved,
            isFalse,
            reason: 'done completed before output drain in cycle $cycle',
          );

          subscription.resume();
          paused = false;
          final done = await doneFuture.timeout(const Duration(seconds: 5));
          await outputDone.future.timeout(const Duration(seconds: 5));

          expect(done, processExit);
          expect(utf8.decode(received), sentinel);
        } catch (error, stackTrace) {
          throw StateError(
            'PTY exit-drain stress failed: cycle=$cycle/$cycles '
            'error=$error\n$stackTrace',
          );
        } finally {
          if (paused) subscription.resume();
          await subscription.cancel();
          await session.close();
        }
      }
    },
    skip: skipReason,
    timeout: const Timeout(Duration(hours: 2)),
  );
}

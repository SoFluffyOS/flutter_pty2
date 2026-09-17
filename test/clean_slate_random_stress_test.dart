import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final fixture = Platform.environment['PTY_TEST_CHILD'];
  final operationLimit = _configuredPositiveInt(
    'PTY_RANDOM_STRESS_OPERATIONS',
    fallback: 1024,
  );
  final maxActionsPerSession = _configuredPositiveInt(
    'PTY_RANDOM_STRESS_MAX_ACTIONS',
    fallback: 32,
  );
  final configured =
      (Platform.isLinux || Platform.isMacOS || Platform.isWindows) &&
          library?.isNotEmpty == true &&
          fixture?.isNotEmpty == true;
  final skipReason = switch (configured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY and PTY_TEST_CHILD on a desktop '
        'to run randomized stress.',
  };

  test(
    'replays seeded lifecycle races without losing cleanup',
    () async {
      final child = fixture;
      if (child == null) return;

      const seed = 38172931;
      final random = math.Random(seed);
      var operationCount = 0;
      var cycle = 0;
      while (operationCount < operationLimit) {
        final session = await Pty.spawn(
          PtySpawnOptions(
            executable: child,
            arguments: const ['slow-output', '65536', '1'],
            inputBufferBytes: 64 * 1024,
            outputWindowBytes: 16 * 1024,
          ),
        );
        final subscription = session.output.listen((_) {});
        var closed = false;
        var paused = false;
        try {
          for (var action = 0;
              action < maxActionsPerSession && operationCount < operationLimit;
              action++) {
            operationCount++;
            try {
              switch (random.nextInt(6)) {
                case 0:
                  session.input.tryWrite(
                    Uint8List.fromList([
                      cycle % 256,
                      action % 256,
                      random.nextInt(256),
                    ]),
                  );
                case 1:
                  session.resize(
                    PtySize(
                      columns: 40 + random.nextInt(160),
                      rows: 10 + random.nextInt(50),
                    ),
                  );
                case 2:
                  switch (paused) {
                    case true:
                      subscription.resume();
                      paused = false;
                    case false:
                      subscription.pause();
                      paused = true;
                  }
                case 3:
                  session.sendSignal(PosixSignal.term);
                case 4:
                  session.kill();
                case 5:
                  await session.close();
                  closed = true;
              }
            } on PtyException {
              // Process exit racing with an operation is expected here.
            }
            if (closed) break;
            await Future<void>.delayed(const Duration(milliseconds: 1));
          }

          final exit = await session.done.timeout(const Duration(seconds: 10));
          expect(exit, isA<PtyExit>());
        } catch (error, stackTrace) {
          throw StateError(
            'PTY random stress failed: seed=$seed operation=$operationCount '
            'cycle=$cycle '
            'error=$error\n$stackTrace',
          );
        } finally {
          if (closed == false) await session.close();
          await subscription.cancel();
        }
        cycle++;
      }
    },
    skip: skipReason,
    timeout: const Timeout(Duration(minutes: 10)),
  );
}

int _configuredPositiveInt(String name, {required int fallback}) {
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

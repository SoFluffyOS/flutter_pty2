// ignore_for_file: deprecated_member_use

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_pty2/src/generated/flutter_pty_bindings_generated.dart'
    as native;
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final configured =
      (Platform.isLinux || Platform.isMacOS || Platform.isWindows) &&
          library?.isNotEmpty == true;
  final skipReason = switch (configured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY on a desktop platform to run spawn-failure '
        'stress.',
  };

  test(
    'releases native state after repeated asynchronous spawn failures',
    () async {
      final libraryPath = library;
      if (libraryPath == null) return;

      final debug = _NativeDebug(libraryPath);
      final baseline = debug.read();
      final executable = switch (Platform.isWindows) {
        true => r'Z:\flutter_pty2_missing\spawn-target.exe',
        false => '/flutter_pty2_missing/spawn-target',
      };
      try {
        for (var attempt = 0; attempt < 32; attempt++) {
          await expectLater(
            Pty.spawn(
              PtySpawnOptions(executable: executable),
            ),
            throwsA(isA<PtySpawnException>()),
          );

          final released = await _waitFor(
            () => debug.read().sameAs(baseline),
          );
          expect(
            released,
            isTrue,
            reason: 'native state leaked after spawn failure $attempt',
          );
        }
      } finally {
        debug.dispose();
      }
    },
    skip: skipReason,
    timeout: const Timeout(Duration(minutes: 2)),
  );
}

Future<bool> _waitFor(
  bool Function() predicate, {
  int attempts = 20,
}) async {
  for (var attempt = 0; attempt < attempts; attempt++) {
    if (predicate()) return true;
    await Future<void>.delayed(const Duration(milliseconds: 10));
  }
  return predicate();
}

final class _NativeDebug {
  _NativeDebug(String path)
      : _bindings = native.FlutterPtyBindings(DynamicLibrary.open(path)),
        _pointer = calloc<native.PtyDebugStats>();

  final native.FlutterPtyBindings _bindings;
  final Pointer<native.PtyDebugStats> _pointer;

  _StatsSnapshot read() {
    _bindings.pty_debug_get_stats(_pointer);
    final stats = _pointer.ref;
    return _StatsSnapshot(
      liveSessions: stats.live_sessions,
      liveReadWorkers: stats.live_read_workers,
      liveWriteWorkers: stats.live_write_workers,
      liveWaitWorkers: stats.live_wait_workers,
      liveCloseWorkers: stats.live_close_workers,
      livePseudoConsoleWorkers: stats.live_pseudo_console_workers,
      pendingWriteChunks: stats.pending_write_chunks,
      pendingWriteBytes: stats.pending_write_bytes,
      inflightWriteBytes: stats.inflight_write_bytes,
    );
  }

  void dispose() => calloc.free(_pointer);
}

final class _StatsSnapshot {
  const _StatsSnapshot({
    required this.liveSessions,
    required this.liveReadWorkers,
    required this.liveWriteWorkers,
    required this.liveWaitWorkers,
    required this.liveCloseWorkers,
    required this.livePseudoConsoleWorkers,
    required this.pendingWriteChunks,
    required this.pendingWriteBytes,
    required this.inflightWriteBytes,
  });

  final int liveSessions;
  final int liveReadWorkers;
  final int liveWriteWorkers;
  final int liveWaitWorkers;
  final int liveCloseWorkers;
  final int livePseudoConsoleWorkers;
  final int pendingWriteChunks;
  final int pendingWriteBytes;
  final int inflightWriteBytes;

  bool sameAs(_StatsSnapshot other) {
    return liveSessions == other.liveSessions &&
        liveReadWorkers == other.liveReadWorkers &&
        liveWriteWorkers == other.liveWriteWorkers &&
        liveWaitWorkers == other.liveWaitWorkers &&
        liveCloseWorkers == other.liveCloseWorkers &&
        livePseudoConsoleWorkers == other.livePseudoConsoleWorkers &&
        pendingWriteChunks == other.pendingWriteChunks &&
        pendingWriteBytes == other.pendingWriteBytes &&
        inflightWriteBytes == other.inflightWriteBytes;
  }

  @override
  String toString() {
    return 'sessions=$liveSessions read=$liveReadWorkers '
        'write=$liveWriteWorkers wait=$liveWaitWorkers close=$liveCloseWorkers '
        'pseudo=$livePseudoConsoleWorkers chunks=$pendingWriteChunks '
        'bytes=$pendingWriteBytes inflight=$inflightWriteBytes';
  }
}

// ignore_for_file: deprecated_member_use

import 'dart:convert';
import 'dart:developer' as developer;
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';
import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_pty2/src/generated/flutter_pty_bindings_generated.dart'
    as native;
import 'package:flutter_test/flutter_test.dart';

void main() {
  final library = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  final fixture = Platform.environment['PTY_TEST_CHILD'];
  final configured = switch (Platform.isWindows) {
    true => library?.isNotEmpty == true && fixture?.isNotEmpty == true,
    false =>
      (Platform.isLinux || Platform.isMacOS) && library?.isNotEmpty == true,
  };
  final skipReason = switch (configured) {
    true => false,
    _ => 'Set FLUTTER_PTY2_LIBRARY and PTY_TEST_CHILD on a desktop '
        'platform to run finalizer tests.',
  };

  test(
    'NativeFinalizer eventually releases an unclosed session',
    () async {
      final libraryPath = library;
      if (libraryPath == null) return;
      final debug = _NativeDebug(libraryPath);
      final baseline = debug.read();
      final references = <WeakReference<PtySession>>[];
      final executable = switch (Platform.isWindows) {
        true => fixture,
        false => '/bin/sh',
      };
      if (executable == null) return;
      final arguments = switch (Platform.isWindows) {
        true => const ['sleep', '30'],
        false => const ['-c', 'sleep 30'],
      };
      try {
        for (var index = 0; index < 4; index++) {
          references.add(
            await _spawnUnownedSession(
              executable: executable,
              arguments: arguments,
            ),
          );
        }
        await _waitFor(
          () => debug.read().liveSessions >= baseline.liveSessions + 4,
        );

        if (!await _requestFullGc()) {
          markTestSkipped(
            'The Dart VM service is required to force finalizer collection.',
          );
          return;
        }

        final released = await _waitFor(
          () => debug.read().sameAs(baseline),
          attempts: 40,
        );
        expect(
          released,
          isTrue,
          reason: 'baseline=$baseline observed=${debug.read()}',
        );
      } finally {
        for (final reference in references) {
          final session = reference.target;
          if (session case final liveSession?) await liveSession.close();
        }
        debug.dispose();
      }
    },
    skip: skipReason,
    timeout: const Timeout(Duration(minutes: 2)),
  );
}

Future<WeakReference<PtySession>> _spawnUnownedSession({
  required String executable,
  required List<String> arguments,
}) async {
  final session = await Pty.spawn(
    PtySpawnOptions(
      executable: executable,
      arguments: arguments,
    ),
  );
  return WeakReference(session);
}

Future<bool> _waitFor(
  bool Function() predicate, {
  int attempts = 20,
}) async {
  for (var attempt = 0; attempt < attempts; attempt++) {
    if (predicate()) return true;
    await Future<void>.delayed(const Duration(milliseconds: 100));
  }
  return predicate();
}

Future<bool> _requestFullGc() async {
  try {
    final info = await developer.Service.controlWebServer(enable: true);
    final uri = info.serverWebSocketUri;
    final isolateId = developer.Service.getIsolateID(Isolate.current);
    if (uri == null || isolateId == null) return false;
    final socket = await WebSocket.connect(uri.toString());
    try {
      socket.add(
        jsonEncode({
          'jsonrpc': '2.0',
          'id': 1,
          'method': 'getAllocationProfile',
          'params': <String, Object>{
            'isolateId': isolateId,
            'gc': true,
          },
        }),
      );
      await socket.first.timeout(const Duration(seconds: 5));
      return true;
    } finally {
      await socket.close();
    }
  } on Object {
    return false;
  }
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

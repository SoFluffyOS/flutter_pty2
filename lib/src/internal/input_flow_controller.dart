import 'dart:async';
import 'dart:collection';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_pty2/src/pty_exception.dart';
import 'package:flutter_pty2/src/pty_input.dart';

final class InputFlowController implements PtyInput {
  InputFlowController({
    required this.nativeTryWrite,
    this.maxChunkSize = 64 * 1024,
  });

  final PtyWriteResult Function(int requestId, Uint8List bytes) nativeTryWrite;
  final int maxChunkSize;
  final Queue<_PendingWrite> _waiting = Queue<_PendingWrite>();
  final Map<int, _PendingWrite> _inflight = <int, _PendingWrite>{};

  int _nextRequestId = 1;
  bool _waitingForWritable = false;
  bool _closed = false;

  @override
  Future<void> write(Uint8List data) async {
    if (_closed) throw const PtyClosedException();
    if (data.isEmpty) return;

    final futures = <Future<void>>[];
    var offset = 0;
    while (offset < data.length) {
      final end = math.min(offset + maxChunkSize, data.length);
      final pending = _PendingWrite(
        id: _nextRequestId++,
        bytes: Uint8List.fromList(data.sublist(offset, end)),
      );
      _waiting.addLast(pending);
      futures.add(pending.completer.future);
      offset = end;
    }
    _pump();
    await Future.wait(futures);
  }

  @override
  PtyWriteResult tryWrite(Uint8List data) {
    if (_closed) return PtyWriteResult.closed;
    if (data.isEmpty) return PtyWriteResult.accepted;
    if (data.length > maxChunkSize) {
      throw ArgumentError.value(
        data.length,
        'data.length',
        'tryWrite accepts at most $maxChunkSize bytes',
      );
    }
    if (_waiting.isNotEmpty || _waitingForWritable) {
      return PtyWriteResult.backpressured;
    }

    final id = _nextRequestId++;
    try {
      final result = nativeTryWrite(id, data);
      if (result == PtyWriteResult.accepted) {
        _inflight[id] = _PendingWrite(id: id, bytes: Uint8List(0));
      }
      return result;
    } catch (error, stackTrace) {
      closeWithError(error, stackTrace);
      return PtyWriteResult.closed;
    }
  }

  @override
  Future<void> flush() {
    final futures = <Future<void>>[
      for (final pending in _waiting) pending.completer.future,
      for (final pending in _inflight.values) pending.completer.future,
    ];
    return Future.wait(futures);
  }

  void handleWritable() {
    if (_closed) return;
    _waitingForWritable = false;
    _pump();
  }

  void handleWriteComplete(int requestId) {
    final pending = _inflight.remove(requestId);
    if (pending == null) return;
    if (!pending.completer.isCompleted) pending.completer.complete();
    _pump();
  }

  void handleClosed(Object error) {
    closeWithError(error, StackTrace.current);
  }

  void closeWithError(Object error, [StackTrace? stackTrace]) {
    if (_closed) return;
    _closed = true;
    for (final pending in _waiting) {
      if (!pending.completer.isCompleted) {
        pending.completer.completeError(error, stackTrace);
      }
    }
    for (final pending in _inflight.values) {
      if (!pending.completer.isCompleted) {
        pending.completer.completeError(error, stackTrace);
      }
    }
    _waiting.clear();
    _inflight.clear();
  }

  void _pump() {
    if (_closed || _waitingForWritable) return;
    while (_waiting.isNotEmpty) {
      final pending = _waiting.first;
      PtyWriteResult result;
      try {
        result = nativeTryWrite(pending.id, pending.bytes);
      } catch (error, stackTrace) {
        closeWithError(error, stackTrace);
        return;
      }
      switch (result) {
        case PtyWriteResult.accepted:
          _waiting.removeFirst();
          _inflight[pending.id] = pending;
        case PtyWriteResult.backpressured:
          _waitingForWritable = true;
          return;
        case PtyWriteResult.closed:
          closeWithError(const PtyClosedException());
          return;
      }
    }
  }
}

final class _PendingWrite {
  _PendingWrite({required this.id, required this.bytes});

  final int id;
  final Uint8List bytes;
  final completer = Completer<void>();
}

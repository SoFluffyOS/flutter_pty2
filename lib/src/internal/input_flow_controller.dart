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
  Object? _owner;
  WeakReference<Object>? _ownerReference;

  void setOwner(Object owner) {
    _ownerReference = WeakReference(owner);
  }

  final Queue<_WriteOperation> _waiting = Queue<_WriteOperation>();
  final Map<int, _PendingWrite> _inflight = <int, _PendingWrite>{};

  int _nextRequestId = 1;
  bool _waitingForWritable = false;
  bool _closed = false;

  @override
  Future<void> write(Uint8List data) async {
    if (_closed) throw const PtyClosedException();
    if (data.isEmpty) return;

    _retainOwner();
    final operation = _WriteOperation(Uint8List.fromList(data));
    _waiting.addLast(operation);
    _pump();
    try {
      await operation.completer.future;
    } finally {
      _releaseOwnerIfIdle();
    }
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

    _retainOwner();
    final id = _nextRequestId++;
    PtyWriteResult result;
    try {
      result = nativeTryWrite(id, data);
      if (result == PtyWriteResult.accepted) {
        _inflight[id] = _PendingWrite(id: id);
      }
    } catch (error, stackTrace) {
      closeWithError(error, stackTrace);
      return PtyWriteResult.closed;
    }
    _releaseOwnerIfIdle();
    return result;
  }

  @override
  Future<void> flush() {
    final futures = <Future<void>>[
      for (final operation in _waiting) operation.completer.future,
      for (final pending in _inflight.values)
        if (pending.operation == null) pending.completer.future,
    ];
    return Future.wait(futures);
  }

  void handleWritable() {
    if (_closed) return;
    _waitingForWritable = false;
    _pump();
    _releaseOwnerIfIdle();
  }

  void handleWriteComplete(int requestId) {
    final pending = _inflight.remove(requestId);
    if (pending == null) return;
    final operation = pending.operation;
    switch (operation) {
      case null:
        if (!pending.completer.isCompleted) pending.completer.complete();
      case final operation:
        operation.inflightCount--;
        if (operation.offset >= operation.data.length &&
            operation.inflightCount == 0) {
          _waiting.removeFirst();
          if (!operation.completer.isCompleted) {
            operation.completer.complete();
          }
        }
    }
    _pump();
    _releaseOwnerIfIdle();
  }

  void handleClosed(Object error) {
    closeWithError(error, StackTrace.current);
  }

  void closeWithError(Object error, [StackTrace? stackTrace]) {
    if (_closed) return;
    _closed = true;
    for (final operation in _waiting) {
      if (!operation.completer.isCompleted) {
        operation.completer.completeError(error, stackTrace);
      }
    }
    for (final pending in _inflight.values) {
      if (pending.operation == null && !pending.completer.isCompleted) {
        pending.completer.completeError(error, stackTrace);
      }
    }
    _waiting.clear();
    _inflight.clear();
    _owner = null;
  }

  void _pump() {
    if (_closed || _waitingForWritable) return;
    while (_waiting.isNotEmpty) {
      final operation = _waiting.first;
      if (operation.offset >= operation.data.length) return;
      final end =
          math.min(operation.offset + maxChunkSize, operation.data.length);
      final chunk = Uint8List(end - operation.offset)
        ..setRange(0, end - operation.offset, operation.data, operation.offset);
      final requestId = operation.requestId ??= _nextRequestId++;
      PtyWriteResult result;
      try {
        result = nativeTryWrite(requestId, chunk);
      } catch (error, stackTrace) {
        closeWithError(error, stackTrace);
        return;
      }
      switch (result) {
        case PtyWriteResult.accepted:
          operation.requestId = null;
          operation.offset = end;
          operation.inflightCount++;
          _inflight[requestId] = _PendingWrite(
            id: requestId,
            operation: operation,
          );
        case PtyWriteResult.backpressured:
          _waitingForWritable = true;
          return;
        case PtyWriteResult.closed:
          closeWithError(const PtyClosedException());
          return;
      }
    }
  }

  void _retainOwner() {
    _owner = _ownerReference?.target;
  }

  void _releaseOwnerIfIdle() {
    final owner = _owner;
    if (owner == null) return;
    if (_waiting.isEmpty && _inflight.isEmpty) _owner = null;
  }
}

final class _PendingWrite {
  _PendingWrite({required this.id, this.operation});

  final int id;
  final _WriteOperation? operation;
  final completer = Completer<void>();
}

final class _WriteOperation {
  _WriteOperation(this.data);

  final Uint8List data;
  final completer = Completer<void>();
  int offset = 0;
  int inflightCount = 0;
  int? requestId;
}

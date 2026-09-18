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
    this.maxPendingBytes = 1024 * 1024,
  }) {
    if (maxChunkSize <= 0) {
      throw ArgumentError.value(
        maxChunkSize,
        'maxChunkSize',
        'must be positive',
      );
    }
    if (maxPendingBytes <= 0) {
      throw ArgumentError.value(
        maxPendingBytes,
        'maxPendingBytes',
        'must be positive',
      );
    }
  }

  final PtyWriteResult Function(int requestId, Uint8List bytes) nativeTryWrite;
  final int maxChunkSize;
  final int maxPendingBytes;
  Object? _owner;
  WeakReference<Object>? _ownerReference;

  void setOwner(Object owner) {
    _ownerReference = WeakReference(owner);
  }

  final Queue<_WriteRequest> _admissions = Queue<_WriteRequest>();
  final Queue<_WriteOperation> _waiting = Queue<_WriteOperation>();
  final Map<int, _PendingWrite> _inflight = <int, _PendingWrite>{};

  int _nextRequestId = 1;
  int _pendingBytes = 0;
  bool _waitingForWritable = false;
  bool _closed = false;

  @override
  Future<void> write(Uint8List data) async {
    if (_closed) throw const PtyClosedException();
    if (data.isEmpty) return;

    _retainOwner();
    final request = _WriteRequest(data);
    request.completion = _waitForOperation(request.admitted.future);
    _admissions.addLast(request);
    _pumpAdmissions();
    try {
      await request.completion;
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
    if (_admissions.isNotEmpty || _waiting.isNotEmpty || _waitingForWritable) {
      return PtyWriteResult.backpressured;
    }

    _retainOwner();
    final id = _nextRequestId++;
    PtyWriteResult result;
    try {
      result = nativeTryWrite(id, data);
      if (result == PtyWriteResult.accepted) {
        final pending = _PendingWrite(id: id);
        _inflight[id] = pending;
        unawaited(pending.completer.future.catchError((Object _) {}));
      }
    } catch (error, stackTrace) {
      closeWithError(error, stackTrace);
      return PtyWriteResult.closed;
    }
    if (result == PtyWriteResult.closed) {
      closeWithError(const PtyClosedException());
    }
    _releaseOwnerIfIdle();
    return result;
  }

  @override
  Future<void> flush() {
    final futures = <Future<void>>[
      for (final request in _admissions) request.completion,
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
          _pendingBytes -= operation.data.length;
          if (!operation.completer.isCompleted) {
            operation.completer.complete();
          }
          _pumpAdmissions();
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
    for (final request in _admissions) {
      if (!request.admitted.isCompleted) {
        request.admitted.completeError(error, stackTrace);
      }
    }
    _admissions.clear();
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
    _pendingBytes = 0;
    _owner = null;
  }

  Future<void> _waitForOperation(
    Future<_WriteOperation> admitted,
  ) async {
    final operation = await admitted;
    await operation.completer.future;
  }

  void _pumpAdmissions() {
    if (_closed) return;
    while (_admissions.isNotEmpty) {
      final request = _admissions.first;
      if (!_canAdmit(request.data.length)) return;
      _admissions.removeFirst();
      final operation = _WriteOperation(Uint8List.fromList(request.data));
      _pendingBytes += operation.data.length;
      _waiting.addLast(operation);
      request.admitted.complete(operation);
      _pump();
    }
  }

  bool _canAdmit(int length) {
    if (length > maxPendingBytes) return _pendingBytes == 0;
    return _pendingBytes <= maxPendingBytes - length;
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
    if (_admissions.isEmpty && _waiting.isEmpty && _inflight.isEmpty) {
      _owner = null;
    }
  }
}

final class _WriteRequest {
  _WriteRequest(this.data);

  final Uint8List data;
  final admitted = Completer<_WriteOperation>();
  late final Future<void> completion;
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

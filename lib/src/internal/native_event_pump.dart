import 'dart:async';
import 'dart:isolate';

import 'package:flutter_pty2/src/internal/native_event.dart';

abstract interface class NativeEventHandler {
  void handleNativeEvent(NativeEvent event);

  void handleProtocolError(String message);
}

final class NativeEventPump {
  NativeEventPump(this._port);

  final ReceivePort _port;
  StreamSubscription<dynamic>? _subscription;
  NativeEventHandler? _handler;
  Future<void>? _closeFuture;
  bool _closed = false;

  void attach(NativeEventHandler handler) {
    if (_subscription != null || _closed) {
      throw StateError('Native event pump is already attached');
    }
    _handler = handler;
    _subscription = _port.listen(_handleMessage);
  }

  Future<void> close() {
    final existing = _closeFuture;
    if (existing != null) return existing;
    final closeFuture = _close();
    _closeFuture = closeFuture;
    return closeFuture;
  }

  Future<void> _close() async {
    _closed = true;
    final subscription = _subscription;
    _subscription = null;
    _handler = null;
    await subscription?.cancel();
    _port.close();
  }

  void _handleMessage(Object? message) {
    final event = _parseEvent(message);
    final handler = _handler;
    if (handler == null) return;
    if (event == null) return;
    handler.handleNativeEvent(event);
    if (event case NativeSessionClosed()) unawaited(close());
  }

  NativeEvent? _parseEvent(Object? message) {
    try {
      return NativeEvent.parse(message);
    } on FormatException catch (error) {
      _handler?.handleProtocolError(error.message);
      return null;
    }
  }
}

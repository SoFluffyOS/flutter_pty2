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

  void attach(NativeEventHandler handler) {
    if (_subscription != null) {
      throw StateError('Native event pump is already attached');
    }
    _handler = handler;
    _subscription = _port.listen(_handleMessage);
  }

  Future<void> close() async {
    await _subscription?.cancel();
    _subscription = null;
    _handler = null;
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

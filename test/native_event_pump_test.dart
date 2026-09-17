import 'dart:isolate';

import 'package:flutter_pty2/src/internal/native_event.dart';
import 'package:flutter_pty2/src/internal/native_event_pump.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('dispatches events and reports protocol errors through one port',
      () async {
    final port = ReceivePort();
    final handler = _RecordingHandler();
    final pump = NativeEventPump(port);
    pump.attach(handler);

    port.sendPort.send(<Object?>[6]);
    port.sendPort.send(<Object?>[999]);
    await Future<void>.delayed(Duration.zero);

    expect(handler.events, hasLength(1));
    expect(handler.events.single, isA<NativeWritable>());
    expect(handler.errors, hasLength(1));
    await pump.close();
  });

  test('rejects attaching a second handler', () {
    final port = ReceivePort();
    final pump = NativeEventPump(port);
    pump.attach(_RecordingHandler());

    expect(
      () => pump.attach(_RecordingHandler()),
      throwsStateError,
    );
    port.close();
  });

  test('close is idempotent when session closure races explicit cleanup',
      () async {
    final port = ReceivePort();
    final pump = NativeEventPump(port);
    pump.attach(_RecordingHandler());

    final firstClose = pump.close();
    final secondClose = pump.close();

    expect(identical(firstClose, secondClose), isTrue);
    await Future.wait([firstClose, secondClose]);
    expect(
      () => pump.attach(_RecordingHandler()),
      throwsStateError,
    );
  });
}

final class _RecordingHandler implements NativeEventHandler {
  final events = <NativeEvent>[];
  final errors = <String>[];

  @override
  void handleNativeEvent(NativeEvent event) {
    events.add(event);
  }

  @override
  void handleProtocolError(String message) {
    errors.add(message);
  }
}

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

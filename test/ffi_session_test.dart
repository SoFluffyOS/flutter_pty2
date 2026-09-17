import 'dart:ffi';
import 'dart:typed_data';

import 'package:flutter_pty2/src/generated/flutter_pty_bindings_generated.dart'
    as native;
import 'package:flutter_pty2/src/internal/ffi_session.dart';
import 'package:flutter_pty2/src/internal/native_event.dart';
import 'package:flutter_pty2/src/pty_exception.dart';
import 'package:flutter_pty2/src/pty_input.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('fails spawn and lifecycle futures on an early async error', () async {
    final state = FfiPtySessionState(
      handle: Pointer<native.PtySession>.fromAddress(1),
      bindings: native.FlutterPtyBindings.fromLookup(_fakeLookup),
    );
    final spawned = state.spawned;
    final processExit = state.processExit;
    final done = state.done;

    state.handleNativeEvent(
      const NativeAsyncError(
        PtyNativeError(
          domain: PtyErrorDomain.posix,
          kind: PtyErrorKind.io,
          code: 5,
          message: 'read failed',
        ),
      ),
    );

    await expectLater(spawned, throwsA(isA<PtyIoException>()));
    await expectLater(processExit, throwsA(isA<PtyIoException>()));
    await expectLater(done, throwsA(isA<PtyIoException>()));
    expect(
      state.input.tryWrite(Uint8List.fromList([1])),
      PtyWriteResult.closed,
    );
  });

  test('fails closed and requests native shutdown on protocol errors',
      () async {
    _protocolCloseCalls = 0;
    _protocolDiscardCalls = 0;
    final state = FfiPtySessionState(
      handle: Pointer<native.PtySession>.fromAddress(1),
      bindings: native.FlutterPtyBindings.fromLookup(_protocolLookup),
    );
    final spawned = state.spawned;
    final processExit = state.processExit;
    final done = state.done;

    state.handleProtocolError('malformed native event');
    state.handleProtocolError('same malformed native event');

    await expectLater(spawned, throwsA(isA<StateError>()));
    await expectLater(processExit, throwsA(isA<StateError>()));
    await expectLater(done, throwsA(isA<StateError>()));
    expect(
      state.input.tryWrite(Uint8List.fromList([1])),
      PtyWriteResult.closed,
    );
    expect(_protocolCloseCalls, 1);
    expect(_protocolDiscardCalls, 1);
  });
}

Pointer<T> _fakeLookup<T extends NativeType>(String symbolName) {
  return Pointer<T>.fromAddress(1);
}

int _protocolCloseCalls = 0;
int _protocolDiscardCalls = 0;

void _protocolBeginClose(Pointer<native.PtySession> session) {
  _protocolCloseCalls++;
}

void _protocolDiscardOutput(Pointer<native.PtySession> session) {
  _protocolDiscardCalls++;
}

Pointer<T> _protocolLookup<T extends NativeType>(String symbolName) {
  if (symbolName == 'pty_session_begin_close') {
    return Pointer.fromFunction<Void Function(Pointer<native.PtySession>)>(
            _protocolBeginClose)
        .cast();
  }
  if (symbolName == 'pty_session_discard_output') {
    return Pointer.fromFunction<Void Function(Pointer<native.PtySession>)>(
      _protocolDiscardOutput,
    ).cast();
  }
  return Pointer<T>.fromAddress(1);
}

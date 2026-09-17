import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:flutter_pty2/src/generated/flutter_pty_bindings_generated.dart'
    as native;
import 'package:flutter_pty2/src/internal/ffi_session.dart';
import 'package:flutter_pty2/src/internal/native_event.dart';
import 'package:flutter_pty2/src/internal/native_event_pump.dart';
import 'package:flutter_pty2/src/pty_exception.dart';
import 'package:flutter_pty2/src/pty_exit.dart';
import 'package:flutter_pty2/src/pty_input.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('fails spawn and lifecycle futures on an early async error', () async {
    var closeCalls = 0;
    final state = FfiPtySessionState(
      handle: Pointer<native.PtySession>.fromAddress(1),
      bindings: native.FlutterPtyBindings.fromLookup(_fakeLookup),
      onAsyncError: () => closeCalls++,
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
    expect(closeCalls, 1);
    expect(
      state.input.tryWrite(Uint8List.fromList([1])),
      PtyWriteResult.closed,
    );
  });

  test('requests native shutdown only once for repeated async errors',
      () async {
    var closeCalls = 0;
    final state = FfiPtySessionState(
      handle: Pointer<native.PtySession>.fromAddress(1),
      bindings: native.FlutterPtyBindings.fromLookup(_fakeLookup),
      onAsyncError: () => closeCalls++,
    );
    const error = NativeAsyncError(
      PtyNativeError(
        domain: PtyErrorDomain.posix,
        kind: PtyErrorKind.io,
        code: 5,
        message: 'read failed',
      ),
    );

    state.handleNativeEvent(error);
    state.handleNativeEvent(error);

    await expectLater(state.spawned, throwsA(isA<PtyIoException>()));
    await expectLater(state.processExit, throwsA(isA<PtyIoException>()));
    await expectLater(state.done, throwsA(isA<PtyIoException>()));
    expect(closeCalls, 1);
  });

  test('does not surface unobserved lifecycle errors', () async {
    final state = FfiPtySessionState(
      handle: Pointer<native.PtySession>.fromAddress(1),
      bindings: native.FlutterPtyBindings.fromLookup(_fakeLookup),
    );

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

    await expectLater(state.spawned, throwsA(isA<PtyIoException>()));
    await Future<void>.delayed(Duration.zero);
  });

  test('requests native shutdown when a native write fails', () {
    _writeErrorCloseCalls = 0;
    final state = FfiPtySessionState(
      handle: Pointer<native.PtySession>.fromAddress(1),
      bindings: native.FlutterPtyBindings.fromLookup(_writeErrorLookup),
    );

    expect(
      state.input.tryWrite(Uint8List.fromList([1])),
      PtyWriteResult.closed,
    );
    expect(_writeErrorCloseCalls, 1);
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

  test('protocol errors win over pending output drain completion', () async {
    _protocolCloseCalls = 0;
    _protocolDiscardCalls = 0;
    final state = FfiPtySessionState(
      handle: Pointer<native.PtySession>.fromAddress(1),
      bindings: native.FlutterPtyBindings.fromLookup(_protocolLookup),
    );
    final spawned = state.spawned;
    final processExit = state.processExit;
    final done = state.done;
    final subscription = state.output.stream.listen((_) {});
    subscription.pause();

    state.handleNativeEvent(
      NativeOutput(Uint8List.fromList([1])),
    );
    state.handleNativeEvent(const NativeOutputClosed());
    state.handleNativeEvent(
      const NativeProcessExit(exit: PtyExitCode(0)),
    );
    state.handleProtocolError('malformed native event');

    final spawnedExpectation = expectLater(
      spawned,
      throwsA(isA<StateError>()),
    );
    final processExpectation = processExit.then((processResult) {
      expect(processResult, isA<PtyExitCode>());
      if (processResult case PtyExitCode(:final code)) expect(code, 0);
    });
    final doneExpectation = expectLater(done, throwsA(isA<StateError>()));
    await Future.wait(
        [spawnedExpectation, processExpectation, doneExpectation]);
    expect(_protocolCloseCalls, 1);
    expect(_protocolDiscardCalls, 1);
    subscription.resume();
    await subscription.cancel();
  });

  test('completes close after a malformed terminal event', () async {
    _protocolCloseCalls = 0;
    _protocolDiscardCalls = 0;
    final port = ReceivePort();
    final state = FfiPtySessionState(
      handle: Pointer<native.PtySession>.fromAddress(1),
      bindings: native.FlutterPtyBindings.fromLookup(_protocolLookup),
    );
    final pump = NativeEventPump(port)..attach(state);

    final closed = state.closed;
    port.sendPort.send(<Object?>[999]);
    port.sendPort.send(<Object?>[10, 'unexpected']);

    await closed;
    expect(_protocolCloseCalls, 1);
    expect(_protocolDiscardCalls, 1);
    await pump.close();
  });
}

Pointer<T> _fakeLookup<T extends NativeType>(String symbolName) {
  return Pointer<T>.fromAddress(1);
}

int _protocolCloseCalls = 0;
int _protocolDiscardCalls = 0;
int _writeErrorCloseCalls = 0;

void _protocolBeginClose(Pointer<native.PtySession> session) {
  _protocolCloseCalls++;
}

void _protocolDiscardOutput(Pointer<native.PtySession> session) {
  _protocolDiscardCalls++;
}

void _protocolAcknowledgeOutput(
  Pointer<native.PtySession> session,
  int byteCount,
) {}

int _writeError(
  Pointer<native.PtySession> session,
  int requestId,
  Pointer<Uint8> bytes,
  int length,
  Pointer<native.PtyError> error,
) {
  error.ref
    ..domain = PtyErrorDomain.internal.index + 1
    ..kind = PtyErrorKind.io.index + 1
    ..os_code = 5;
  return native.PtyTryWriteResult.PTY_WRITE_ERROR;
}

void _writeErrorBeginClose(Pointer<native.PtySession> session) {
  _writeErrorCloseCalls++;
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
  if (symbolName == 'pty_session_ack_output') {
    return Pointer.fromFunction<
        Void Function(Pointer<native.PtySession>, Uint64)>(
      _protocolAcknowledgeOutput,
    ).cast();
  }
  return Pointer<T>.fromAddress(1);
}

Pointer<T> _writeErrorLookup<T extends NativeType>(String symbolName) {
  if (symbolName == 'pty_session_try_write') {
    return Pointer.fromFunction<
            Int32 Function(
              Pointer<native.PtySession>,
              Uint64,
              Pointer<Uint8>,
              Uint64,
              Pointer<native.PtyError>,
            )>(_writeError, native.PtyTryWriteResult.PTY_WRITE_ERROR)
        .cast();
  }
  if (symbolName == 'pty_session_begin_close') {
    return Pointer.fromFunction<Void Function(Pointer<native.PtySession>)>(
      _writeErrorBeginClose,
    ).cast();
  }
  return Pointer<T>.fromAddress(1);
}

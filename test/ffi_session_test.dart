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
}

Pointer<T> _fakeLookup<T extends NativeType>(String symbolName) {
  return Pointer<T>.fromAddress(1);
}

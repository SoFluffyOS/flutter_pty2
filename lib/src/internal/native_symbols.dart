// The C functions keep their native names.
// ignore_for_file: non_constant_identifier_names

/// The native library built by `hook/build.dart`, bundled as a code asset.
@DefaultAsset('package:flutter_pty2/flutter_pty2')
library;

import 'dart:ffi';

import 'package:flutter_pty2/src/generated/flutter_pty_bindings_generated.dart'
    as native;

@Native<
  Int32 Function(
    Pointer<native.PtySpawnOptions>,
    Pointer<Pointer<native.PtySession>>,
    Pointer<native.PtyError>,
  )
>()
external int pty_session_start(
  Pointer<native.PtySpawnOptions> options,
  Pointer<Pointer<native.PtySession>> session,
  Pointer<native.PtyError> error,
);

@Native<Int64 Function(Pointer<native.PtySession>)>()
external int pty_session_pid(Pointer<native.PtySession> session);

@Native<
  Int32 Function(
    Pointer<native.PtySession>,
    Uint64,
    Pointer<Uint8>,
    Uint64,
    Pointer<native.PtyError>,
  )
>()
external int pty_session_try_write(
  Pointer<native.PtySession> session,
  int generation,
  Pointer<Uint8> data,
  int length,
  Pointer<native.PtyError> error,
);

@Native<Void Function(Pointer<native.PtySession>, Uint64)>()
external void pty_session_ack_output(
  Pointer<native.PtySession> session,
  int bytes,
);

@Native<
  Int32 Function(
    Pointer<native.PtySession>,
    native.PtySize,
    Pointer<native.PtyError>,
  )
>()
external int pty_session_resize(
  Pointer<native.PtySession> session,
  native.PtySize size,
  Pointer<native.PtyError> error,
);

@Native<Int32 Function(Pointer<native.PtySession>, Pointer<native.PtyError>)>()
external int pty_session_kill(
  Pointer<native.PtySession> session,
  Pointer<native.PtyError> error,
);

@Native<
  Int32 Function(
    Pointer<native.PtySession>,
    Int32,
    Int32,
    Pointer<native.PtyError>,
  )
>()
external int pty_session_send_signal(
  Pointer<native.PtySession> session,
  int signal,
  int target,
  Pointer<native.PtyError> error,
);

@Native<Void Function(Pointer<native.PtySession>)>()
external void pty_session_discard_output(Pointer<native.PtySession> session);

@Native<Void Function(Pointer<native.PtySession>)>()
external void pty_session_begin_close(Pointer<native.PtySession> session);

@Native<Void Function(Pointer<native.PtySession>)>()
external void pty_session_release(Pointer<native.PtySession> session);

@Native<Void Function(Pointer<Void>)>()
external void pty_session_abandon(Pointer<Void> session);

@Native<Void Function(Pointer<native.PtyDebugStats>)>()
external void pty_debug_get_stats(Pointer<native.PtyDebugStats> stats);

@Native<IntPtr Function(Pointer<Void>)>()
external int Dart_InitializeApiDL(Pointer<Void> data);

/// Looks up [symbolName] in the bundled native library.
Pointer<T> lookupBundledSymbol<T extends NativeType>(String symbolName) {
  final Pointer<NativeType> address = switch (symbolName) {
    'pty_session_start' =>
      Native.addressOf<
        NativeFunction<
          Int32 Function(
            Pointer<native.PtySpawnOptions>,
            Pointer<Pointer<native.PtySession>>,
            Pointer<native.PtyError>,
          )
        >
      >(pty_session_start),
    'pty_session_pid' =>
      Native.addressOf<
        NativeFunction<Int64 Function(Pointer<native.PtySession>)>
      >(pty_session_pid),
    'pty_session_try_write' =>
      Native.addressOf<
        NativeFunction<
          Int32 Function(
            Pointer<native.PtySession>,
            Uint64,
            Pointer<Uint8>,
            Uint64,
            Pointer<native.PtyError>,
          )
        >
      >(pty_session_try_write),
    'pty_session_ack_output' =>
      Native.addressOf<
        NativeFunction<Void Function(Pointer<native.PtySession>, Uint64)>
      >(pty_session_ack_output),
    'pty_session_resize' =>
      Native.addressOf<
        NativeFunction<
          Int32 Function(
            Pointer<native.PtySession>,
            native.PtySize,
            Pointer<native.PtyError>,
          )
        >
      >(pty_session_resize),
    'pty_session_kill' =>
      Native.addressOf<
        NativeFunction<
          Int32 Function(Pointer<native.PtySession>, Pointer<native.PtyError>)
        >
      >(pty_session_kill),
    'pty_session_send_signal' =>
      Native.addressOf<
        NativeFunction<
          Int32 Function(
            Pointer<native.PtySession>,
            Int32,
            Int32,
            Pointer<native.PtyError>,
          )
        >
      >(pty_session_send_signal),
    'pty_session_discard_output' =>
      Native.addressOf<
        NativeFunction<Void Function(Pointer<native.PtySession>)>
      >(pty_session_discard_output),
    'pty_session_begin_close' =>
      Native.addressOf<
        NativeFunction<Void Function(Pointer<native.PtySession>)>
      >(pty_session_begin_close),
    'pty_session_release' =>
      Native.addressOf<
        NativeFunction<Void Function(Pointer<native.PtySession>)>
      >(pty_session_release),
    'pty_session_abandon' =>
      Native.addressOf<NativeFunction<Void Function(Pointer<Void>)>>(
        pty_session_abandon,
      ),
    'pty_debug_get_stats' =>
      Native.addressOf<
        NativeFunction<Void Function(Pointer<native.PtyDebugStats>)>
      >(pty_debug_get_stats),
    'Dart_InitializeApiDL' =>
      Native.addressOf<NativeFunction<IntPtr Function(Pointer<Void>)>>(
        Dart_InitializeApiDL,
      ),
    _ => throw ArgumentError.value(
      symbolName,
      'symbolName',
      'is not exported by flutter_pty2',
    ),
  };
  return address.cast<T>();
}

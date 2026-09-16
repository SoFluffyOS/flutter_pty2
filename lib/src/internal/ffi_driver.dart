import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:flutter_pty2/src/generated/flutter_pty_bindings_generated.dart'
    as native;
import 'package:flutter_pty2/src/internal/input_flow_controller.dart';
import 'package:flutter_pty2/src/internal/native_event.dart';
import 'package:flutter_pty2/src/internal/native_event_pump.dart';
import 'package:flutter_pty2/src/internal/output_flow_controller.dart';
import 'package:flutter_pty2/src/pty_capabilities.dart';
import 'package:flutter_pty2/src/pty_environment.dart';
import 'package:flutter_pty2/src/pty_exception.dart';
import 'package:flutter_pty2/src/pty_exit.dart';
import 'package:flutter_pty2/src/pty_input.dart';
import 'package:flutter_pty2/src/pty_session.dart';
import 'package:flutter_pty2/src/pty_size.dart';
import 'package:flutter_pty2/src/pty_spawn_options.dart';

final class FfiPtyDriver {
  FfiPtyDriver._();

  static final instance = FfiPtyDriver._();
  static final DynamicLibrary _library = _openLibrary();
  static final native.FlutterPtyBindings _bindings =
      native.FlutterPtyBindings(_library);
  static final NativeFinalizer _sessionFinalizer = NativeFinalizer(
    _library.lookup<NativeFunction<Void Function(Pointer<Void>)>>(
      'pty_session_abandon',
    ),
  );
  static bool _initialized = false;

  Future<PtySession> spawn(PtySpawnOptions options) async {
    options.validate();
    _ensureInitialized();

    final environment = buildEnvironment(
      options.environment,
      caseInsensitive: Platform.isWindows,
    );
    final port = ReceivePort();
    late final Pointer<native.PtySession> nativeSession;
    try {
      nativeSession = using((arena) {
        final nativeOptions = arena.allocate<native.PtySpawnOptions>(
          sizeOf<native.PtySpawnOptions>(),
        );
        final executable = options.executable.toNativeUtf8(allocator: arena);
        final arguments = arena<Pointer<Char>>(options.arguments.length + 1);
        for (var index = 0; index < options.arguments.length; index++) {
          (arguments + index).value =
              options.arguments[index].toNativeUtf8(allocator: arena).cast();
        }
        (arguments + options.arguments.length).value = nullptr;

        final environmentValues = environment.entries
            .map((entry) => '${entry.key}=${entry.value}')
            .toList(growable: false);
        final environmentPointers =
            arena<Pointer<Char>>(environmentValues.length + 1);
        for (var index = 0; index < environmentValues.length; index++) {
          (environmentPointers + index).value =
              environmentValues[index].toNativeUtf8(allocator: arena).cast();
        }
        (environmentPointers + environmentValues.length).value = nullptr;

        nativeOptions.ref
          ..executable = executable.cast()
          ..arguments = arguments
          ..argument_count = options.arguments.length
          ..environment = environmentPointers
          ..environment_count = environmentValues.length
          ..working_directory = switch (options.workingDirectory) {
            final directory? => directory.toNativeUtf8(allocator: arena).cast(),
            null => nullptr,
          }
          ..input_buffer_bytes = options.inputBufferBytes
          ..output_window_bytes = options.outputWindowBytes
          ..event_port = port.sendPort.nativePort;
        nativeOptions.ref.size
          ..rows = options.size.rows
          ..columns = options.size.columns
          ..pixel_width = options.size.pixelWidth
          ..pixel_height = options.size.pixelHeight;

        final outSession = arena<Pointer<native.PtySession>>();
        final outError = arena<native.PtyError>();
        final result = _bindings.pty_session_start(
          nativeOptions,
          outSession,
          outError,
        );
        if (result == 0 || outSession.value == nullptr) {
          throw PtySpawnException(
            'Starting PTY session failed',
            nativeError: _readNativeError(outError.ref),
          );
        }
        return outSession.value;
      });
    } catch (_) {
      port.close();
      rethrow;
    }

    final session = _FfiPtySession(
      handle: nativeSession,
      bindings: _bindings,
      port: port,
      finalizer: _sessionFinalizer,
    );
    session.attach();
    try {
      return await session.waitForSpawn();
    } catch (_) {
      unawaited(session.close());
      rethrow;
    }
  }

  void _ensureInitialized() {
    if (_initialized) return;
    final result = _bindings.Dart_InitializeApiDL(
      NativeApi.initializeApiDLData.cast(),
    );
    if (result != 0) {
      throw StateError('Failed to initialize native PTY bindings');
    }
    _initialized = true;
  }
}

DynamicLibrary _openLibrary() {
  final override = Platform.environment['FLUTTER_PTY2_LIBRARY'];
  if (override case final path? when path.isNotEmpty) {
    return DynamicLibrary.open(path);
  }
  if (Platform.isMacOS || Platform.isIOS) return DynamicLibrary.process();
  if (Platform.isLinux || Platform.isAndroid) {
    return DynamicLibrary.open('libflutter_pty2.so');
  }
  if (Platform.isWindows) return DynamicLibrary.open('flutter_pty2.dll');
  throw UnsupportedError('Unknown platform: ${Platform.operatingSystem}');
}

final class _FfiPtySessionState implements NativeEventHandler {
  _FfiPtySessionState({
    required this.handle,
    required this.bindings,
  }) {
    _output = OutputFlowController(
      acknowledge: _acknowledgeOutput,
      discardOutput: _discardOutput,
      onDrained: _handleOutputDrained,
    );
    _input = InputFlowController(nativeTryWrite: _tryWrite);
  }

  final Pointer<native.PtySession> handle;
  final native.FlutterPtyBindings bindings;
  final _spawnCompleter = Completer<void>();
  final _processExitCompleter = Completer<PtyExit>();
  final _doneCompleter = Completer<PtyExit>();
  final _closedCompleter = Completer<void>();
  late final OutputFlowController _output;
  late final InputFlowController _input;
  PtyExit? _processExit;
  bool _outputClosed = false;
  bool _outputDrained = false;

  OutputFlowController get output => _output;

  InputFlowController get input => _input;

  Future<void> get spawned => _spawnCompleter.future;

  Future<PtyExit> get processExit => _processExitCompleter.future;

  Future<PtyExit> get done => _doneCompleter.future;

  Future<void> get closed => _closedCompleter.future;

  void _acknowledgeOutput(int bytes) {
    bindings.pty_session_ack_output(handle, bytes);
  }

  void _discardOutput() {
    bindings.pty_session_discard_output(handle);
  }

  void _handleOutputDrained() {
    _outputDrained = true;
    _maybeCompleteDone();
  }

  @override
  void handleNativeEvent(NativeEvent event) {
    switch (event) {
      case NativeSpawned():
        if (!_spawnCompleter.isCompleted) _spawnCompleter.complete();
      case NativeSpawnFailed(:final error):
        if (!_spawnCompleter.isCompleted) {
          _spawnCompleter.completeError(
            PtySpawnException('PTY process failed to spawn',
                nativeError: error),
          );
        }
      case NativeOutput(:final bytes):
        _output.addNativeOutput(bytes);
      case NativeOutputClosed():
        if (_outputClosed) return;
        _outputClosed = true;
        _output.handleNativeClosed();
        _maybeCompleteDone();
      case NativeWriteComplete(:final requestId):
        _input.handleWriteComplete(requestId);
      case NativeWritable():
        _input.handleWritable();
      case NativeInputClosed(:final error):
        _input.handleClosed(
            PtyIoException('PTY input closed', nativeError: error));
      case NativeProcessExit(:final exit):
        if (_processExit != null) return;
        _processExit = exit;
        _processExitCompleter.complete(exit);
        _maybeCompleteDone();
      case NativeAsyncError(:final error):
        _input
            .handleClosed(PtyIoException('PTY I/O failed', nativeError: error));
      case NativeSessionClosed():
        if (!_closedCompleter.isCompleted) _closedCompleter.complete();
    }
  }

  @override
  void handleProtocolError(String message) {
    final error = StateError(message);
    if (!_spawnCompleter.isCompleted) _spawnCompleter.completeError(error);
    if (!_processExitCompleter.isCompleted) {
      _processExitCompleter.completeError(error);
    }
    if (!_doneCompleter.isCompleted) _doneCompleter.completeError(error);
  }

  void _maybeCompleteDone() {
    final exit = _processExit;
    if (exit == null ||
        !_outputClosed ||
        !_outputDrained ||
        _doneCompleter.isCompleted) {
      return;
    }
    _doneCompleter.complete(exit);
  }

  PtyWriteResult _tryWrite(int requestId, Uint8List data) {
    return using((arena) {
      final nativeBytes = arena<Uint8>(data.length);
      nativeBytes.asTypedList(data.length).setAll(0, data);
      final error = arena<native.PtyError>();
      final result = bindings.pty_session_try_write(
        handle,
        requestId,
        nativeBytes,
        data.length,
        error,
      );
      if (result == native.PtyTryWriteResult.PTY_WRITE_ERROR) {
        throw PtyIoException('Writing to PTY failed',
            nativeError: _readNativeError(error.ref));
      }
      return switch (result) {
        native.PtyTryWriteResult.PTY_WRITE_ACCEPTED => PtyWriteResult.accepted,
        native.PtyTryWriteResult.PTY_WRITE_BACKPRESSURED =>
          PtyWriteResult.backpressured,
        _ => PtyWriteResult.closed,
      };
    });
  }
}

final class _FfiPtySession implements PtySession, Finalizable {
  _FfiPtySession({
    required this.handle,
    required this.bindings,
    required this.port,
    required NativeFinalizer finalizer,
  })  : _finalizer = finalizer,
        _state = _FfiPtySessionState(
          handle: handle,
          bindings: bindings,
        );

  final Pointer<native.PtySession> handle;
  final native.FlutterPtyBindings bindings;
  final ReceivePort port;
  final NativeFinalizer _finalizer;
  final _FfiPtySessionState _state;
  final _finalizerDetachToken = Object();
  late final NativeEventPump _eventPump;
  bool _closing = false;
  bool _finalizerDetached = false;

  @override
  int get pid {
    _ensureOpen();
    return bindings.pty_session_pid(handle).toInt();
  }

  @override
  PtyCapabilities get capabilities {
    if (Platform.isWindows) {
      return const PtyCapabilities(
        posixSignals: false,
        foregroundProcessGroups: false,
        pixelDimensions: false,
        reliableProcessTreeKill: true,
        conPty: true,
      );
    }
    return const PtyCapabilities(
      posixSignals: true,
      foregroundProcessGroups: true,
      pixelDimensions: true,
      reliableProcessTreeKill: false,
      conPty: false,
    );
  }

  @override
  Stream<Uint8List> get output {
    _state.output.setOwner(this);
    return _state.output.stream;
  }

  @override
  PtyInput get input {
    _state.input.setOwner(this);
    return _state.input;
  }

  @override
  Future<PtyExit> get processExit =>
      _state.processExit.then(_retainSessionUntilFutureCompletes);

  @override
  Future<PtyExit> get done =>
      _state.done.then(_retainSessionUntilFutureCompletes);

  void attach() {
    _eventPump = NativeEventPump(port)..attach(_state);
    _finalizer.attach(
      this,
      handle.cast(),
      detach: _finalizerDetachToken,
    );
  }

  Future<PtySession> waitForSpawn() async {
    await _state.spawned;
    return this;
  }

  PtyExit _retainSessionUntilFutureCompletes(PtyExit exit) => exit;

  @override
  void resize(PtySize size) {
    _ensureOpen();
    size.validate();
    using((arena) {
      final nativeSize = arena<native.PtySize>();
      nativeSize.ref
        ..rows = size.rows
        ..columns = size.columns
        ..pixel_width = size.pixelWidth
        ..pixel_height = size.pixelHeight;
      final error = arena<native.PtyError>();
      final result = bindings.pty_session_resize(handle, nativeSize.ref, error);
      if (result == 0) {
        throw PtyIoException('Resizing PTY failed',
            nativeError: _readNativeError(error.ref));
      }
    });
  }

  @override
  void kill() {
    _ensureOpen();
    using((arena) {
      final error = arena<native.PtyError>();
      final result = bindings.pty_session_kill(handle, error);
      if (result == 0) {
        throw PtyIoException('Killing PTY failed',
            nativeError: _readNativeError(error.ref));
      }
    });
  }

  @override
  void sendSignal(PosixSignal signal,
      {PosixSignalTarget target = PosixSignalTarget.foregroundProcessGroup}) {
    _ensureOpen();
    if (Platform.isWindows) {
      throw const PtyUnsupportedException(
        'POSIX signals are unsupported on Windows',
      );
    }
    using((arena) {
      final error = arena<native.PtyError>();
      final result = bindings.pty_session_send_signal(
        handle,
        signal.number,
        target.index,
        error,
      );
      if (result == 0) {
        throw PtyIoException('Sending POSIX signal failed',
            nativeError: _readNativeError(error.ref));
      }
    });
  }

  @override
  Future<void> close() async {
    if (_closing) return _state.closed;
    _closing = true;
    _state.input.closeWithError(const PtyClosedException());
    bindings.pty_session_begin_close(handle);
    await _state.closed;
    _state.output.closeAndDiscard();
    if (!_finalizerDetached) {
      _finalizer.detach(_finalizerDetachToken);
      _finalizerDetached = true;
      bindings.pty_session_release(handle);
    }
    await _eventPump.close();
  }

  void _ensureOpen() {
    if (_closing) throw const PtyClosedException();
  }
}

PtyNativeError _readNativeError(native.PtyError error) {
  final codeUnits = <int>[];
  for (var index = 0; index < 256; index++) {
    final value = error.message[index];
    if (value == 0) break;
    codeUnits.add(value);
  }
  final domain = _enumValue(PtyErrorDomain.values, error.domain);
  final kind = _enumValue(PtyErrorKind.values, error.kind);
  return PtyNativeError(
    domain: domain,
    kind: kind,
    code: error.os_code,
    message: String.fromCharCodes(codeUnits),
  );
}

T _enumValue<T>(List<T> values, int index) {
  if (index <= 0 || index > values.length) {
    throw StateError('Unknown native enum value: $index');
  }
  return values[index - 1];
}

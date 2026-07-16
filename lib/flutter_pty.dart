import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:flutter_pty2/src/flutter_pty_bindings_generated.dart';
import 'package:flutter_pty2/src/environment.dart';
import 'package:flutter_pty2/src/options_validation.dart';

const _libName = 'flutter_pty2';

final DynamicLibrary _dylib = () {
  if (Platform.isMacOS || Platform.isIOS) {
    return DynamicLibrary.open('$_libName.framework/$_libName');
  }
  if (Platform.isAndroid || Platform.isLinux) {
    return DynamicLibrary.open('lib$_libName.so');
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open('$_libName.dll');
  }
  throw UnsupportedError('Unknown platform: ${Platform.operatingSystem}');
}();

final _bindings = FlutterPtyBindings(_dylib);

final _init = () {
  return _bindings.Dart_InitializeApiDL(NativeApi.initializeApiDLData);
}();

void _ensureInitialized() {
  if (_init != 0) {
    throw StateError('Failed to initialize native bindings');
  }
}

/// Pty represents a process running in a pseudo-terminal.
///
/// To create a Pty, use [Pty.start].
class Pty {
  final String executable;

  final List<String> arguments;

  /// Spawns a process in a pseudo-terminal. The arguments have the same meaning
  /// as in [Process.start].
  /// [ackRead] indicates if the pty should wait for a call to [Pty.ackRead] before sending the next data.
  Pty.start(
    this.executable, {
    this.arguments = const [],
    String? workingDirectory,
    Map<String, String>? environment,
    String? terminalProgramVersion,
    int rows = 25,
    int columns = 80,
    bool ackRead = false,
  }) {
    validatePtyStartOptions(
      executable: executable,
      arguments: arguments,
      workingDirectory: workingDirectory,
      environment: environment,
      terminalProgramVersion: terminalProgramVersion,
      rows: rows,
      columns: columns,
    );
    _ensureInitialized();

    final caseInsensitiveEnvironment = Platform.isWindows;
    final effectiveEnv = buildPtyEnvironment(
      Platform.environment,
      environment,
      caseInsensitive: caseInsensitiveEnvironment,
      terminalProgramVersion: terminalProgramVersion,
    );
    final environmentEntries = orderPtyEnvironment(
      effectiveEnv,
      caseInsensitive: caseInsensitiveEnvironment,
    );

    // build argv
    final argv = calloc<Pointer<Utf8>>(arguments.length + 2);
    argv.value = executable.toNativeUtf8();
    for (var i = 0; i < arguments.length; i++) {
      (argv + i + 1).value = arguments[i].toNativeUtf8();
    }
    (argv + arguments.length + 1).value = nullptr;

    //build env
    final envp = calloc<Pointer<Utf8>>(environmentEntries.length + 1);
    var environmentIndex = 0;
    for (final entry in environmentEntries) {
      (envp + environmentIndex).value =
          '${entry.key}=${entry.value}'.toNativeUtf8();
      environmentIndex++;
    }
    (envp + environmentEntries.length).value = nullptr;

    final options = calloc<PtyOptions>();
    options.ref.rows = rows;
    options.ref.cols = columns;
    options.ref.executable = executable.toNativeUtf8().cast();
    options.ref.arguments = argv.cast();
    options.ref.environment = envp.cast();
    options.ref.stdout_port = _stdoutPort.sendPort.nativePort;
    options.ref.exit_port = _exitPort.sendPort.nativePort;
    options.ref.output_done_port = _stdoutPort.sendPort.nativePort;
    options.ref.ackRead = ackRead;

    if (workingDirectory != null) {
      options.ref.working_directory = workingDirectory.toNativeUtf8().cast();
    } else {
      options.ref.working_directory = nullptr;
    }

    _handle = _bindings.pty_create(options);

    malloc.free(options.ref.executable);
    if (options.ref.working_directory != nullptr) {
      malloc.free(options.ref.working_directory);
    }
    for (var i = 0; i < arguments.length + 1; i++) {
      malloc.free((argv + i).value);
    }
    calloc.free(argv);
    for (var i = 0; i < environmentEntries.length; i++) {
      malloc.free((envp + i).value);
    }
    calloc.free(envp);
    calloc.free(options);

    if (_handle == nullptr) {
      final error = _getPtyError();
      _stdoutPort.close();
      _exitPort.close();
      unawaited(_outputController.close());
      throw StateError('Failed to create PTY: $error');
    }

    _stdoutPort.listen(_onNativeOutput);
    _exitPort.listen(_onNativeExit);
  }

  final _stdoutPort = ReceivePort();

  final _exitPort = ReceivePort();

  final _outputController = StreamController<Uint8List>(sync: true);

  late final Stream<Uint8List> _output = _outputController.stream;

  final _exitCodeCompleter = Completer<int>();

  late final Pointer<PtyHandle> _handle;

  bool _isDestroyed = false;

  int? _nativeExitCode;

  bool _isOutputDone = false;

  /// The output stream from the pseudo-terminal. Note that pseudo-terminals
  /// do not distinguish between stdout and stderr.
  Stream<Uint8List> get output => _output;

  /// A `Future` which completes with the exit code of the process
  /// when the process completes.
  ///
  /// The handling of exit codes is platform specific.
  ///
  /// On Linux and OS X a normal exit code will be a positive value in
  /// the range `[0..255]`. If the process was terminated due to a signal
  /// the exit code will be a negative value in the range `[-255..-1]`,
  /// where the absolute value of the exit code is the signal
  /// number. For example, if a process crashes due to a segmentation
  /// violation the exit code will be -11, as the signal SIGSEGV has the
  /// number 11.
  ///
  /// On Windows a process can report any 32-bit value as an exit
  /// code. When returning the exit code this exit code is turned into
  /// a signed value. Some special values are used to report
  /// termination due to some system event. E.g. if a process crashes
  /// due to an access violation the 32-bit exit code is `0xc0000005`,
  /// which will be returned as the negative number `-1073741819`. To
  /// get the original 32-bit value use `(0x100000000 + exitCode) &
  /// 0xffffffff`.
  ///
  /// There is no guarantee that [output] have finished reporting the buffered
  /// output of the process when the returned future completes.
  /// To be sure that all output is captured, wait for the done event on the
  /// streams.
  Future<int> get exitCode => _exitCodeCompleter.future;

  /// The process id of the process running in the pseudo-terminal.
  int get pid {
    if (_isDestroyed) {
      throw StateError('PTY has been destroyed');
    }
    return _bindings.pty_getpid(_handle);
  }

  /// Whether the shell currently has a running foreground command.
  ///
  /// Unix platforms compare the PTY foreground process group with the shell's
  /// process group. Windows reports whether the ConPTY shell has a live child
  /// process, since ConPTY does not expose Unix-style foreground job control.
  bool get hasRunningForegroundProcess {
    if (_isDestroyed) return false;
    return _bindings.pty_has_running_foreground_process(_handle) != 0;
  }

  /// Write data to the pseudo-terminal.
  ///
  /// Returns false if the process has been destroyed or native backpressure
  /// rejected the write.
  bool write(Uint8List data) {
    if (_isDestroyed || data.isEmpty) return false;
    final buf = malloc<Int8>(data.length);
    buf.asTypedList(data.length).setAll(0, data);
    final written = _bindings.pty_write(_handle, buf.cast(), data.length) != 0;
    malloc.free(buf);
    return written;
  }

  /// Resize the pseudo-terminal.
  void resize(
    int rows,
    int cols, {
    int pixelWidth = 0,
    int pixelHeight = 0,
  }) {
    if (_isDestroyed) return;
    validatePtySize(
      rows: rows,
      columns: cols,
      pixelWidth: pixelWidth,
      pixelHeight: pixelHeight,
    );
    final result = _bindings.pty_resize(
      _handle,
      rows,
      cols,
      pixelWidth,
      pixelHeight,
    );
    if (result != 0) {
      final error = _getPtyError() ?? 'Unknown native error';
      throw StateError('Failed to resize PTY: $error');
    }
  }

  /// Kill the process running in the pseudo-terminal.
  ///
  /// When possible, [signal] will be sent to the process. This includes
  /// Linux and OS X. The default signal is [ProcessSignal.sigterm]
  /// which will normally terminate the process.
  bool kill([ProcessSignal signal = ProcessSignal.sigterm]) {
    if (_isDestroyed) return false;
    return _bindings.pty_kill(_handle, signal.signalNumber) != 0;
  }

  /// indicates that a data chunk has been processed.
  /// This is needed when ackRead is set to true as the pty will wait for this signal to happen
  /// before any additional data is sent.
  void ackRead() {
    if (_isDestroyed) return;
    _bindings.pty_ack_read(_handle);
  }

  void _onNativeExit(dynamic exitCode) {
    _nativeExitCode = exitCode as int;
    _completeExitAfterOutputDrain();
  }

  void _onNativeOutput(dynamic message) {
    if (message is Uint8List) {
      _outputController.add(message);
      return;
    }

    _isOutputDone = true;
    unawaited(_outputController.close());
    _completeExitAfterOutputDrain();
  }

  void _completeExitAfterOutputDrain() {
    final exitCode = _nativeExitCode;
    if (!_isOutputDone || exitCode == null || _exitCodeCompleter.isCompleted) {
      return;
    }
    _stdoutPort.close();
    _exitPort.close();
    _exitCodeCompleter.complete(exitCode);
  }

  /// Destroys the PTY handle, closing the master fd and freeing native resources.
  /// This should be called when the terminal is disposed to ensure full cleanup.
  void destroy() {
    if (_isDestroyed) return;
    _isDestroyed = true;
    _bindings.pty_destroy(_handle);
    _stdoutPort.close();
    _exitPort.close();
    unawaited(_outputController.close());
    if (!_exitCodeCompleter.isCompleted) {
      _exitCodeCompleter.complete(_nativeExitCode ?? -1);
    }
  }
}

String? _getPtyError() {
  final error = _bindings.pty_error();

  if (error == nullptr) {
    return null;
  }

  return error.cast<Utf8>().toDartString();
}

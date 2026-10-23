import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_pty2/src/pty_capabilities.dart';
import 'package:flutter_pty2/src/pty_exit.dart';
import 'package:flutter_pty2/src/pty_input.dart';
import 'package:flutter_pty2/src/pty_size.dart';

abstract interface class PtySession {
  int get pid;

  /// Whether a program the shell started holds the terminal.
  ///
  /// Unix compares the terminal's foreground process group with the
  /// program's. Windows has no foreground group, so it reports whether the
  /// program has a live child process.
  bool get hasForegroundProcess;

  PtyCapabilities get capabilities;

  Stream<Uint8List> get output;

  PtyInput get input;

  Future<PtyExit> get processExit;

  /// Completes after the child exits and all native output is delivered.
  ///
  /// After this future completes, no more output bytes are emitted.
  Future<PtyExit> get done;

  void resize(PtySize size);

  void kill();

  void sendSignal(
    PosixSignal signal, {
    PosixSignalTarget target = PosixSignalTarget.foregroundProcessGroup,
  });

  Future<void> close();
}

enum PosixSignal {
  hup(1),
  int_(2),
  quit(3),
  kill(9),
  term(15);

  const PosixSignal(this.number);

  final int number;
}

enum PosixSignalTarget {
  shellProcess,
  shellProcessGroup,
  foregroundProcessGroup,
}

extension PtyOutputTextExtension on PtySession {
  Stream<String> utf8Output({bool allowMalformed = true}) {
    return output.cast<List<int>>().transform(
      Utf8Decoder(allowMalformed: allowMalformed),
    );
  }
}

import 'dart:typed_data';

import 'package:flutter_pty2/src/pty_exception.dart';
import 'package:flutter_pty2/src/pty_exit.dart';

sealed class NativeEvent {
  const NativeEvent();

  factory NativeEvent.parse(Object? message) {
    if (message is! List<Object?> || message.isEmpty) {
      throw const FormatException('Native event must be a non-empty list');
    }
    final eventType = _readInt(message, 0);
    return switch (eventType) {
      1 => NativeSpawned(
          pid: _readInt(message, 1),
          capabilities: _readInt(message, 2),
        ),
      2 => NativeSpawnFailed(_readError(message, 1)),
      3 => NativeOutput(_readBytes(message, 1)),
      4 => const NativeOutputClosed(),
      5 => NativeWriteComplete(_readInt(message, 1)),
      6 => const NativeWritable(),
      7 => NativeInputClosed(_readError(message, 1)),
      8 => NativeProcessExit(
          exit: _readExit(message, 1),
        ),
      9 => NativeAsyncError(_readError(message, 1)),
      10 => const NativeSessionClosed(),
      _ => throw FormatException('Unknown native event type: $eventType'),
    };
  }
}

final class NativeSpawned extends NativeEvent {
  const NativeSpawned({required this.pid, required this.capabilities});

  final int pid;
  final int capabilities;
}

final class NativeSpawnFailed extends NativeEvent {
  const NativeSpawnFailed(this.error);

  final PtyNativeError error;
}

final class NativeOutput extends NativeEvent {
  const NativeOutput(this.bytes);

  final Uint8List bytes;
}

final class NativeOutputClosed extends NativeEvent {
  const NativeOutputClosed();
}

final class NativeWriteComplete extends NativeEvent {
  const NativeWriteComplete(this.requestId);

  final int requestId;
}

final class NativeWritable extends NativeEvent {
  const NativeWritable();
}

final class NativeInputClosed extends NativeEvent {
  const NativeInputClosed(this.error);

  final PtyNativeError error;
}

final class NativeProcessExit extends NativeEvent {
  const NativeProcessExit({required this.exit});

  final PtyExit exit;
}

final class NativeAsyncError extends NativeEvent {
  const NativeAsyncError(this.error);

  final PtyNativeError error;
}

final class NativeSessionClosed extends NativeEvent {
  const NativeSessionClosed();
}

int _readInt(List<Object?> message, int index) {
  if (index >= message.length) {
    throw FormatException('Native event field $index must be an integer');
  }
  if (message[index] case final int value) return value;
  throw FormatException('Native event field $index must be an integer');
}

Uint8List _readBytes(List<Object?> message, int index) {
  if (index >= message.length) {
    throw FormatException('Native event field $index must be Uint8List');
  }
  if (message[index] case final Uint8List bytes) return bytes;
  throw FormatException('Native event field $index must be Uint8List');
}

PtyNativeError _readError(List<Object?> message, int index) {
  final domain = _readInt(message, index);
  final kind = _readInt(message, index + 1);
  final code = _readInt(message, index + 2);
  final messageText = message.length > index + 3 ? message[index + 3] : null;
  if (messageText case final String text) {
    return PtyNativeError(
      domain: _enumValue(PtyErrorDomain.values, domain, 'error domain'),
      kind: _enumValue(PtyErrorKind.values, kind, 'error kind'),
      code: code,
      message: text,
    );
  }
  throw const FormatException('Native error message must be a string');
}

PtyExit _readExit(List<Object?> message, int index) {
  final kind = _readInt(message, index);
  final value = _readInt(message, index + 1);
  return switch (kind) {
    0 => PtyExitCode(value),
    1 => PtySignalExit(value),
    _ => throw FormatException('Unknown process exit kind: $kind'),
  };
}

T _enumValue<T>(List<T> values, int index, String name) {
  if (index < 0 || index >= values.length) {
    throw FormatException('Unknown $name: $index');
  }
  return values[index];
}

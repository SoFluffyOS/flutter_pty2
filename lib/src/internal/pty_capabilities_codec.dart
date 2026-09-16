import 'package:flutter_pty2/src/pty_capabilities.dart';

PtyCapabilities decodePtyCapabilities(int bits) {
  return PtyCapabilities(
    posixSignals: bits & 1 != 0,
    foregroundProcessGroups: bits & 2 != 0,
    pixelDimensions: bits & 4 != 0,
    reliableProcessTreeKill: bits & 8 != 0,
    conPty: bits & 16 != 0,
  );
}

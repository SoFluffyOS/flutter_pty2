import 'package:flutter_pty2/src/internal/pty_capabilities_codec.dart';
import 'package:flutter_pty2/src/generated/flutter_pty_bindings_generated.dart'
    as native;
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('decodes Unix capability bits', () {
    final capabilities = decodePtyCapabilities(
      native.PtyCapability.PTY_CAPABILITY_POSIX_SIGNALS |
          native.PtyCapability.PTY_CAPABILITY_FOREGROUND_PROCESS_GROUPS |
          native.PtyCapability.PTY_CAPABILITY_PIXEL_DIMENSIONS,
    );

    expect(capabilities.posixSignals, isTrue);
    expect(capabilities.foregroundProcessGroups, isTrue);
    expect(capabilities.pixelDimensions, isTrue);
    expect(capabilities.reliableProcessTreeKill, isFalse);
    expect(capabilities.conPty, isFalse);
  });

  test('decodes Windows capability bits', () {
    final capabilities = decodePtyCapabilities(
      native.PtyCapability.PTY_CAPABILITY_RELIABLE_PROCESS_TREE_KILL |
          native.PtyCapability.PTY_CAPABILITY_CONPTY,
    );

    expect(capabilities.posixSignals, isFalse);
    expect(capabilities.foregroundProcessGroups, isFalse);
    expect(capabilities.pixelDimensions, isFalse);
    expect(capabilities.reliableProcessTreeKill, isTrue);
    expect(capabilities.conPty, isTrue);
  });
}

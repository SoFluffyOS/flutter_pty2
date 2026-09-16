import 'package:flutter_pty2/src/internal/pty_capabilities_codec.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('decodes Unix capability bits', () {
    final capabilities = decodePtyCapabilities(0x07);

    expect(capabilities.posixSignals, isTrue);
    expect(capabilities.foregroundProcessGroups, isTrue);
    expect(capabilities.pixelDimensions, isTrue);
    expect(capabilities.reliableProcessTreeKill, isFalse);
    expect(capabilities.conPty, isFalse);
  });

  test('decodes Windows capability bits', () {
    final capabilities = decodePtyCapabilities(0x18);

    expect(capabilities.posixSignals, isFalse);
    expect(capabilities.foregroundProcessGroups, isFalse);
    expect(capabilities.pixelDimensions, isFalse);
    expect(capabilities.reliableProcessTreeKill, isTrue);
    expect(capabilities.conPty, isTrue);
  });
}

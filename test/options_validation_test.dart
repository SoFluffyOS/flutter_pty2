import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_pty2/src/options_validation.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('validatePtyStartOptions', () {
    test('accepts valid options', () {
      expect(
        () => validatePtyStartOptions(
          executable: '/bin/sh',
          arguments: const ['-lc', 'true'],
          workingDirectory: '/tmp',
          environment: const {'TERM': 'xterm-256color'},
          rows: 24,
          columns: 80,
        ),
        returnsNormally,
      );
    });

    test('rejects empty executable', () {
      expect(
        () => validatePtyStartOptions(
          executable: '',
          arguments: const [],
          workingDirectory: null,
          environment: null,
          rows: 24,
          columns: 80,
        ),
        throwsArgumentError,
      );
    });

    test('rejects NUL bytes before native allocation', () {
      expect(
        () => validatePtyStartOptions(
          executable: '/bin/sh',
          arguments: const ['bad\x00arg'],
          workingDirectory: null,
          environment: const {'TERM': 'xterm-256color'},
          rows: 24,
          columns: 80,
        ),
        throwsArgumentError,
      );
      expect(
        () => validatePtyStartOptions(
          executable: '/bin/sh',
          arguments: const [],
          workingDirectory: '/tmp\x00',
          environment: null,
          rows: 24,
          columns: 80,
        ),
        throwsArgumentError,
      );
      expect(
        () => validatePtyStartOptions(
          executable: '/bin/sh',
          arguments: const [],
          workingDirectory: null,
          environment: null,
          terminalProgramVersion: '1.0\x00',
          rows: 24,
          columns: 80,
        ),
        throwsArgumentError,
      );
    });

    test('rejects empty terminal program version', () {
      expect(
        () => validatePtyStartOptions(
          executable: '/bin/sh',
          arguments: const [],
          workingDirectory: null,
          environment: null,
          terminalProgramVersion: '',
          rows: 24,
          columns: 80,
        ),
        throwsArgumentError,
      );
    });

    test('rejects invalid environment entries', () {
      expect(
        () => validatePtyStartOptions(
          executable: '/bin/sh',
          arguments: const [],
          workingDirectory: null,
          environment: const {'BAD=KEY': 'value'},
          rows: 24,
          columns: 80,
        ),
        throwsArgumentError,
      );
      expect(
        () => validatePtyStartOptions(
          executable: '/bin/sh',
          arguments: const [],
          workingDirectory: null,
          environment: const {'BAD': 'value\x00'},
          rows: 24,
          columns: 80,
        ),
        throwsArgumentError,
      );
    });

    test('rejects non-positive dimensions', () {
      expect(
        () => validatePtyStartOptions(
          executable: '/bin/sh',
          arguments: const [],
          workingDirectory: null,
          environment: null,
          rows: 0,
          columns: 80,
        ),
        throwsArgumentError,
      );
      expect(
        () => validatePtyStartOptions(
          executable: '/bin/sh',
          arguments: const [],
          workingDirectory: null,
          environment: null,
          rows: 24,
          columns: 0,
        ),
        throwsArgumentError,
      );
    });

    test('rejects dimensions outside native ranges', () {
      expect(
        () => validatePtySize(rows: 0x8000, columns: 80),
        throwsArgumentError,
      );
      expect(
        () => validatePtySize(rows: 24, columns: 0x8000),
        throwsArgumentError,
      );
      expect(
        () => validatePtySize(
          rows: 24,
          columns: 80,
          pixelWidth: -1,
        ),
        throwsArgumentError,
      );
      expect(
        () => validatePtySize(
          rows: 24,
          columns: 80,
          pixelHeight: 0x10000,
        ),
        throwsArgumentError,
      );
    });
  });

  test('Pty.start validates before native initialization', () {
    for (var attempt = 0; attempt < 128; attempt++) {
      expect(() => Pty.start(''), throwsArgumentError);
    }
  });
}

import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_pty2/flutter_pty2.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('PtySize', () {
    test('accepts valid character and pixel dimensions', () {
      const size = PtySize(
        columns: 120,
        rows: 40,
        pixelWidth: 1920,
        pixelHeight: 1080,
      );

      expect(size.validate, returnsNormally);
    });

    test('rejects dimensions outside native ranges', () {
      expect(
        () => const PtySize(columns: 0, rows: 24).validate(),
        throwsArgumentError,
      );
      expect(
        () => const PtySize(columns: 80, rows: 24, pixelWidth: -1).validate(),
        throwsArgumentError,
      );
      expect(
        () => const PtySize(columns: 80, rows: 24, pixelHeight: 0x10000)
            .validate(),
        throwsArgumentError,
      );
    });
  });

  group('PtyEnvironment', () {
    test('supports replacement and Windows key normalization', () {
      const environment = PtyEnvironment.replace({
        'Path': 'base',
        'PATH': 'override',
        'EMPTY': '',
      });

      expect(
        buildEnvironment(environment, caseInsensitive: true),
        {'PATH': 'override', 'EMPTY': ''},
      );
    });

    test('rejects malformed keys and values before native conversion', () {
      expect(
        () => const PtyEnvironment.replace({'BAD=KEY': 'value'}).validate(),
        throwsArgumentError,
      );
      expect(
        () => const PtyEnvironment.replace({'GOOD': 'bad\x00value'}).validate(),
        throwsArgumentError,
      );
    });
  });

  group('PtySpawnOptions', () {
    test('uses the specified bounded-buffer options', () {
      const options = PtySpawnOptions(
        executable: '/bin/sh',
        inputBufferBytes: 64 * 1024,
        outputWindowBytes: 16 * 1024,
      );

      expect(options.validate, returnsNormally);
    });

    test('rejects buffers below the specified minimums', () {
      expect(
        () => const PtySpawnOptions(
          executable: '/bin/sh',
          inputBufferBytes: 63 * 1024,
        ).validate(),
        throwsArgumentError,
      );
      expect(
        () => const PtySpawnOptions(
          executable: '/bin/sh',
          outputWindowBytes: 15 * 1024,
        ).validate(),
        throwsArgumentError,
      );
    });
  });

  test('encodes input text as UTF-8 bytes', () async {
    final input = _RecordingInput();

    await input.writeUtf8('你好🙂');

    expect(input.bytes, Uint8List.fromList(utf8.encode('你好🙂')));
  });
}

final class _RecordingInput implements PtyInput {
  final bytes = <int>[];

  @override
  Future<void> write(Uint8List data) async {
    bytes.addAll(data);
  }

  @override
  PtyWriteResult tryWrite(Uint8List data) {
    bytes.addAll(data);
    return PtyWriteResult.accepted;
  }

  @override
  Future<void> flush() async {}
}

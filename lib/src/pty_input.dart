import 'dart:convert';
import 'dart:typed_data';

enum PtyWriteResult { accepted, backpressured, closed }

abstract interface class PtyInput {
  Future<void> write(Uint8List data);

  PtyWriteResult tryWrite(Uint8List data);

  Future<void> flush();
}

extension PtyInputTextExtension on PtyInput {
  Future<void> writeUtf8(String text) {
    return write(Uint8List.fromList(utf8.encode(text)));
  }
}

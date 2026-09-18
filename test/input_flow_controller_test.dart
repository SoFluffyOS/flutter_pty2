import 'dart:typed_data';

import 'package:flutter_pty2/src/internal/input_flow_controller.dart';
import 'package:flutter_pty2/src/pty_exception.dart';
import 'package:flutter_pty2/src/pty_input.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('serializes chunks and resumes after native backpressure', () async {
    final requests = <int>[];
    var backpressured = true;
    final input = InputFlowController(
      maxChunkSize: 3,
      nativeTryWrite: (requestId, bytes) {
        requests.add(requestId);
        if (requestId == 2 && backpressured) {
          return PtyWriteResult.backpressured;
        }
        return PtyWriteResult.accepted;
      },
    );

    final write = input.write(Uint8List.fromList([1, 2, 3, 4, 5, 6, 7]));
    expect(requests, [1, 2]);
    input.handleWriteComplete(1);
    expect(requests, [1, 2]);
    backpressured = false;
    input.handleWritable();
    expect(requests, [1, 2, 2, 3]);
    input.handleWriteComplete(2);
    input.handleWriteComplete(3);
    await write;
  });

  test('flush waits only for writes submitted before it', () async {
    final requests = <int>[];
    final input = InputFlowController(
      nativeTryWrite: (requestId, _) {
        requests.add(requestId);
        return PtyWriteResult.accepted;
      },
    );
    expect(input.tryWrite(Uint8List.fromList([1])), PtyWriteResult.accepted);
    final flush = input.flush();
    expect(input.tryWrite(Uint8List.fromList([2])), PtyWriteResult.accepted);
    var completed = false;
    flush.then((_) => completed = true);
    await Future<void>.delayed(Duration.zero);
    expect(completed, isFalse);
    input.handleWriteComplete(1);
    await flush;
    expect(requests, [1, 2]);
    expect(completed, isTrue);
    input.handleWriteComplete(2);
  });

  test('keeps separate asynchronous writes ordered', () async {
    final requests = <int>[];
    final input = InputFlowController(
      maxChunkSize: 2,
      nativeTryWrite: (requestId, _) {
        requests.add(requestId);
        return PtyWriteResult.accepted;
      },
    );

    final first = input.write(Uint8List.fromList([1, 2, 3, 4]));
    final second = input.write(Uint8List.fromList([5, 6]));
    expect(requests, [1, 2]);

    input.handleWriteComplete(1);
    expect(requests, [1, 2]);
    input.handleWriteComplete(2);
    expect(requests, [1, 2, 3]);
    input.handleWriteComplete(3);

    await Future.wait([first, second]);
  });

  test('limits admitted asynchronous writes to the pending byte window',
      () async {
    final requests = <int>[];
    final input = InputFlowController(
      maxChunkSize: 2,
      maxPendingBytes: 4,
      nativeTryWrite: (requestId, _) {
        requests.add(requestId);
        return PtyWriteResult.accepted;
      },
    );

    final first = input.write(Uint8List.fromList([1, 2, 3, 4]));
    final second = input.write(Uint8List.fromList([5, 6]));
    final flush = input.flush();

    expect(requests, [1, 2]);
    input.handleWriteComplete(1);
    expect(requests, [1, 2]);
    input.handleWriteComplete(2);
    expect(requests, [1, 2, 3]);

    var flushed = false;
    flush.then((_) => flushed = true);
    await Future<void>.delayed(Duration.zero);
    expect(flushed, isFalse);

    input.handleWriteComplete(3);
    await Future.wait([first, second, flush]);
  });

  test('fails writes waiting for Dart-side admission when closed', () async {
    final input = InputFlowController(
      maxPendingBytes: 2,
      nativeTryWrite: (_, __) => PtyWriteResult.accepted,
    );
    final first = input.write(Uint8List.fromList([1, 2]));
    final second = input.write(Uint8List.fromList([3, 4]));
    final firstExpectation = expectLater(
      first,
      throwsA(isA<PtyClosedException>()),
    );
    final secondExpectation = expectLater(
      second,
      throwsA(isA<PtyClosedException>()),
    );

    input.closeWithError(const PtyClosedException());

    await Future.wait([firstExpectation, secondExpectation]);
  });

  test('snapshots data before retrying after native backpressure', () async {
    final accepted = <Uint8List>[];
    var backpressured = true;
    final input = InputFlowController(
      maxChunkSize: 2,
      nativeTryWrite: (_, bytes) {
        if (accepted.length == 1 && backpressured) {
          return PtyWriteResult.backpressured;
        }
        accepted.add(Uint8List.fromList(bytes));
        return PtyWriteResult.accepted;
      },
    );
    final data = Uint8List.fromList([1, 2, 3, 4]);
    final write = input.write(data);
    data.setAll(0, [9, 9, 9, 9]);

    backpressured = false;
    input.handleWritable();
    input.handleWriteComplete(1);
    input.handleWriteComplete(2);
    await write;

    expect(accepted, [
      Uint8List.fromList([1, 2]),
      Uint8List.fromList([3, 4]),
    ]);
  });

  test('closes all pending writes with the same error', () async {
    final input = InputFlowController(
      nativeTryWrite: (_, __) => PtyWriteResult.accepted,
    );
    final write = input.write(Uint8List.fromList([1]));
    input.handleClosed(const PtyClosedException());
    await expectLater(write, throwsA(isA<PtyClosedException>()));
    expect(input.tryWrite(Uint8List.fromList([2])), PtyWriteResult.closed);
  });

  test('handles an unobserved tryWrite completion failure', () async {
    final input = InputFlowController(
      nativeTryWrite: (_, __) => PtyWriteResult.accepted,
    );
    expect(input.tryWrite(Uint8List.fromList([1])), PtyWriteResult.accepted);
    input.handleClosed(const PtyClosedException());

    await Future<void>.delayed(Duration.zero);
  });

  test('remembers a closed result without retrying native writes', () async {
    var nativeCalls = 0;
    final input = InputFlowController(
      nativeTryWrite: (_, __) {
        nativeCalls++;
        return PtyWriteResult.closed;
      },
    );

    expect(
      input.tryWrite(Uint8List.fromList([1])),
      PtyWriteResult.closed,
    );
    expect(
      input.tryWrite(Uint8List.fromList([2])),
      PtyWriteResult.closed,
    );
    expect(nativeCalls, 1);
    await expectLater(
      input.write(Uint8List.fromList([3])),
      throwsA(isA<PtyClosedException>()),
    );
  });

  test('fails inflight tryWrite requests when a later request is closed',
      () async {
    var nativeCalls = 0;
    final input = InputFlowController(
      nativeTryWrite: (_, __) {
        nativeCalls++;
        if (nativeCalls == 1) return PtyWriteResult.accepted;
        return PtyWriteResult.closed;
      },
    );

    expect(input.tryWrite(Uint8List.fromList([1])), PtyWriteResult.accepted);
    final flush = input.flush();
    expect(input.tryWrite(Uint8List.fromList([2])), PtyWriteResult.closed);

    await expectLater(flush, throwsA(isA<PtyClosedException>()));
  });

  test('tryWrite rejects buffers larger than one native request', () {
    final input = InputFlowController(
      maxChunkSize: 2,
      nativeTryWrite: (_, __) => PtyWriteResult.accepted,
    );
    expect(
      () => input.tryWrite(Uint8List.fromList([1, 2, 3])),
      throwsArgumentError,
    );
  });
}

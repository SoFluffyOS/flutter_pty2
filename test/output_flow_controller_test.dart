import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_pty2/src/internal/output_flow_controller.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('acknowledges output when the listener receives it', () async {
    final acknowledgements = <int>[];
    final controller = OutputFlowController(
      acknowledge: acknowledgements.add,
      discardOutput: () {},
    );
    final received = <Uint8List>[];
    final subscription = controller.stream.listen(received.add);

    final bytes = Uint8List.fromList([1, 2, 3]);
    controller.addNativeOutput(bytes);
    await Future<void>.delayed(Duration.zero);

    expect(received, [bytes]);
    expect(acknowledgements, [3]);
    await subscription.cancel();
  });

  test('buffers while paused and drains in order on resume', () async {
    final acknowledgements = <int>[];
    final controller = OutputFlowController(
      acknowledge: acknowledgements.add,
      discardOutput: () {},
    );
    final received = <int>[];
    final subscription = controller.stream.listen(
      (bytes) => received.add(bytes.single),
    );
    subscription.pause();

    controller.addNativeOutput(Uint8List.fromList([1]));
    controller.addNativeOutput(Uint8List.fromList([2]));
    expect(received, isEmpty);
    expect(acknowledgements, isEmpty);

    subscription.resume();
    await Future<void>.delayed(Duration.zero);

    expect(received, [1, 2]);
    expect(acknowledgements, [1, 1]);
    await subscription.cancel();
  });

  test('acknowledges and discards pending output on cancellation', () async {
    final acknowledgements = <int>[];
    var discarded = false;
    final controller = OutputFlowController(
      acknowledge: acknowledgements.add,
      discardOutput: () => discarded = true,
    );
    final subscription = controller.stream.listen((_) {});
    subscription.pause();
    controller.addNativeOutput(Uint8List.fromList([1, 2]));

    await subscription.cancel();

    expect(acknowledgements, [2]);
    expect(discarded, isTrue);
  });

  test('closes only after pending output has drained', () async {
    final controller = OutputFlowController(
      acknowledge: (_) {},
      discardOutput: () {},
    );
    final received = <int>[];
    final done = Completer<void>();
    final subscription = controller.stream.listen(
      (bytes) => received.add(bytes.single),
      onDone: done.complete,
    );
    subscription.pause();
    controller.addNativeOutput(Uint8List.fromList([7]));
    controller.handleNativeClosed();

    expect(done.isCompleted, isFalse);
    subscription.resume();
    await done.future;

    expect(received, [7]);
    await subscription.cancel();
  });
}

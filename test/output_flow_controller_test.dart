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
    final drained = Completer<void>();
    final controller = OutputFlowController(
      acknowledge: (_) {},
      discardOutput: () {},
      onDrained: drained.complete,
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
    expect(drained.isCompleted, isFalse);
    subscription.resume();
    await done.future;
    await drained.future;

    expect(received, [7]);
    await subscription.cancel();
  });

  test('closeAndDiscard releases buffered output and closes the stream',
      () async {
    final acknowledgements = <int>[];
    var discarded = false;
    final controller = OutputFlowController(
      acknowledge: acknowledgements.add,
      discardOutput: () => discarded = true,
    );
    controller.addNativeOutput(Uint8List.fromList([1, 2, 3]));

    controller.closeAndDiscard();
    final done = Completer<void>();
    final subscription = controller.stream.listen(
      (_) => fail('discarded output was delivered'),
      onDone: done.complete,
    );
    await done.future;

    expect(acknowledgements, [3]);
    expect(discarded, isTrue);
    await subscription.cancel();
  });

  test('closeAndDiscard disarms callbacks after native release', () async {
    final acknowledgements = <int>[];
    var discardCount = 0;
    final controller = OutputFlowController(
      acknowledge: acknowledgements.add,
      discardOutput: () => discardCount++,
    );
    final subscription = controller.stream.listen((_) {});

    controller.closeAndDiscard();
    controller.addNativeOutput(Uint8List.fromList([4]));
    await subscription.cancel();

    expect(acknowledgements, isEmpty);
    expect(discardCount, 1);
  });

  test('acknowledges and drops output received after native close', () async {
    final acknowledgements = <int>[];
    final controller = OutputFlowController(
      acknowledge: acknowledgements.add,
      discardOutput: () {},
    );
    final subscription = controller.stream.listen((_) {
      fail('output received after native close');
    });

    controller.handleNativeClosed();
    controller.addNativeOutput(Uint8List.fromList([5]));
    await subscription.cancel();

    expect(acknowledgements, [1]);
  });
}

import 'dart:async';
import 'dart:collection';
import 'dart:typed_data';

final class OutputFlowController {
  OutputFlowController({
    required void Function(int bytes) acknowledge,
    required void Function() discardOutput,
  })  : _acknowledge = acknowledge,
        _discardOutput = discardOutput {
    _controller = StreamController<Uint8List>(
      sync: true,
      onListen: _handleListen,
      onPause: _handlePause,
      onResume: _handleResume,
      onCancel: _handleCancel,
    );
  }

  final void Function(int bytes) _acknowledge;
  final void Function() _discardOutput;
  final Queue<Uint8List> _pending = Queue<Uint8List>();
  late final StreamController<Uint8List> _controller;

  bool _hasListener = false;
  bool _paused = false;
  bool _cancelled = false;
  bool _nativeClosed = false;

  Stream<Uint8List> get stream => _controller.stream;

  void addNativeOutput(Uint8List bytes) {
    if (_cancelled) {
      _acknowledge(bytes.length);
      return;
    }
    if (!_hasListener || _paused) {
      _pending.addLast(bytes);
      return;
    }
    _controller.add(bytes);
    _acknowledge(bytes.length);
  }

  void handleNativeClosed() {
    _nativeClosed = true;
    _drain();
  }

  Future<void> close() async {
    _nativeClosed = true;
    _drain();
    await _controller.close();
  }

  void _handleListen() {
    _hasListener = true;
    _drain();
  }

  void _handlePause() {
    _paused = true;
  }

  void _handleResume() {
    _paused = false;
    _drain();
  }

  Future<void> _handleCancel() async {
    _cancelled = true;
    while (_pending.isNotEmpty) {
      _acknowledge(_pending.removeFirst().length);
    }
    _discardOutput();
  }

  void _drain() {
    while (_hasListener && !_paused && !_cancelled && _pending.isNotEmpty) {
      final bytes = _pending.removeFirst();
      _controller.add(bytes);
      _acknowledge(bytes.length);
    }
    if (_nativeClosed && _pending.isEmpty && !_controller.isClosed) {
      unawaited(_controller.close());
    }
  }
}

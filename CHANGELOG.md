## (Unreleased)

* Add the clean-slate `Pty.spawn` session API with raw-byte output, async input,
  bounded flow control, typed errors, process lifecycle futures, signals, and
  asynchronous idempotent close.
* Split clean-slate Dart session orchestration and Windows spawn code into
  focused native and Dart drivers.
* Propagate early asynchronous native failures to spawn, process-exit, done,
  and pending-input futures.
* Fail closed and request native shutdown when the clean-slate event protocol is
  malformed.
* Preserve protocol failures when buffered output is still awaiting drain.
* Complete inflight input futures when a later native write observes closure.
* Treat failed native event delivery as Dart endpoint abandonment and release
  the session owner exactly once.
* Skip subsequent native event posts after a session has been abandoned.
* Add an example-app Android integration harness and run it on the emulator in
  native-quality CI.
* Verify clean-slate Android output and input integration on an API 35 emulator.
* Add scheduled macOS AddressSanitizer and UndefinedBehaviorSanitizer coverage.
* Make Windows discard output natively after the Dart consumer cancels.
* Avoid reporting normal Windows ConPTY EOF as an asynchronous I/O error.
* Reject native writes as soon as session shutdown begins.
* Validate terminal dimensions in the native ABI before platform-specific casts.
* Keep PTY output available after an input-channel failure and release queued
  native writes immediately.
* Close sessions safely when shutdown races native startup publication.
* Publish the native platform backend atomically across startup-close races.
* Avoid cross-thread `errno` handoff during concurrent Unix spawns.
* Reset inherited `SIGBUS` handlers in Unix PTY children.
* Report Windows process-wait failures without fabricating an exit code.
* Return typed invalid-argument errors for malformed native input writes.
* Close Unix input permanently when a partial write cannot be requeued.
* Drop Unix output read concurrently canceled by the Dart consumer.
* Preserve typed working-directory errors before relative executable lookup.
* Serialize Unix process reaping with signals and termination requests.
* Preserve HRESULT values in Windows ConPTY spawn and resize errors.
* Use a deterministic error code for zero-byte Windows ConPTY reads.
* Preserve not-found classification for failures reported by Unix `execve`.
* Add native lifecycle, transfer, exit-drain, randomized race, finalizer, and
  platform resource-leak coverage, including scheduled 1 GiB transfers and
  10,000 randomized operations.
* Document clean-slate development commands, benchmarks, native architecture,
  ownership, shutdown, and platform boundaries.
* Report host-process CPU time and normalized utilization for idle and loaded
  concurrency benchmarks.
* Avoid signaling reaped Unix process IDs and make Windows termination and
  spawn failures idempotent and typed.
* Align the minimum Flutter SDK constraint with the Dart 3 implementation.
* Preserve buffered Unix output when a child exits during read backpressure.
* Add Swift Package Manager support for iOS and macOS.

## 1.0.2

* Prevent Unix PTY wake descriptors from leaking into child processes.

## 1.0.1

* Preserve trailing PTY output and reject writes after output closes.
* Surface resize and child startup failures reliably.
* Harden Unix environment, UTF-8 input, and concurrent native errors.
* Improve ConPTY shutdown and contain Windows process trees.

## 1.0.0

* Rename package to `flutter_pty2` for the maintained fork.
* Add explicit `TERM_PROGRAM_VERSION` support for shell integrations.
* Replace Unix cross-thread mutex unlocking with a poll/wakeup event loop.
* Drain PTY output before reporting process exit.
* Clean up native spawn allocations and reader thread resources.
* Handle partial and interrupted Unix writes.
* Queue Unix input through the nonblocking PTY event loop.
* Propagate terminal pixel dimensions during PTY resize.
* Report Unix child setup failures synchronously and close all forked PTY descriptors.
* Preserve the full process environment, advertise true color, enable `IUTF8`, and reset child signals.
* Harden ConPTY ownership, remove startup latency, and support quoted Unicode process arguments.
* Sanitize inherited terminal-emulator identity and provide a UTF-8 locale fallback.

## 0.4.2
* Fix Linux compile error, thanks [@mengyanshou].

## 0.4.1
* Fix compile warning, thanks [@mengyanshou].

## 0.4.0
* Update to Dart3

## 0.3.1
* Update deps

## 0.3.0

* Fixes ignored working directory parameter for Unix [#3], thanks [@devmil].
* Support setting Windows environmental variable and working directory.

## 0.2.0

* Add optional read acknowledge [#2], thanks [@devmil].

## 0.1.1

* Update README

## 0.1.0

* Windows support.
* Support getting exit code

## 0.0.7

* Work on Linux #1
* Work on Android

## 0.0.6

* Flutter >=2.12.0

## 0.0.5

* Fix README syntax

## 0.0.4

* Support resizing of the pty

## 0.0.3

* Support passing env vars
## 0.0.2

* Support passing arguments
## 0.0.1

* Initial release

[#2]: https://github.com/TerminalStudio/flutter_pty/pull/2
[#3]: https://github.com/TerminalStudio/flutter_pty/pull/3

[@devmil]: https://github.com/devmil
[@mengyanshou]: https://github.com/mengyanshou

# `flutter_pty2` architecture

This document describes the clean-slate API exported by
`package:flutter_pty2/flutter_pty2.dart`. The compatibility API exported by
`package:flutter_pty2/flutter_pty.dart` retains the original manual-acknowledgement
model and is intentionally outside this design.

## Runtime boundary

The Dart isolate never performs a blocking PTY operation. Each clean-slate
session owns one `ReceivePort`; native workers post small event messages to
that port, and Dart turns those events into the public session, output, and
input APIs.

```text
Pty.spawn(options)
        |
        v
  FfiPtyDriver ---- one ReceivePort ---- NativeEventPump
        |                                      |
        |                                      +--> OutputFlowController
        |                                      +--> InputFlowController
        |                                      +--> processExit / done
        v
  PtySession (native ref-counted state)
        |
        +--> Unix: poll reactor + process waiter + close worker
        |
        +--> Windows: reader + writer + waiter + close workers
```

Native event payloads contain raw bytes or structured scalar values. Dart
performs UTF-8 decoding only through the optional `utf8Output()` extension;
the primary `output` stream remains `Uint8List` so binary data is preserved.

## Session ownership and shutdown

Native session memory is reference counted. The Dart session owns the initial
reference, and every native worker retains the session until that worker has
finished. The finalizer is attached to the Dart wrapper and calls the native
abandon entry point. Abandonment marks the session, requests asynchronous
shutdown, and releases Dart's reference; it does not join workers or wait for
a process.

The platform backend pointer is published atomically by the bootstrap worker.
Close and finalizer paths acquire that pointer before using it, so a close
during startup cannot race pointer publication.

Every native event post checks its success result. A failed post means the Dart
endpoint is gone, so native code enters the same abandonment path. Abandonment
is idempotent: a failed event post and the later `NativeFinalizer` callback can
race without releasing the Dart-owned reference twice.

Explicit `close()` follows this sequence:

```text
close()
  |
  +--> mark closing and stop accepting input
  +--> start the native close worker
  +--> terminate the PTY-owned process/session
  +--> wake and finish native workers
  +--> post SESSION_CLOSED
  +--> detach NativeFinalizer and release Dart's reference
  +--> close the Dart event pump
```

`close()` is idempotent. It is allowed to discard output still buffered in
Dart. Callers that need all output must await `done` first. `processExit`
reports child termination; `done` waits for both that result and output EOF
plus delivery of all output already accepted by Dart.

## Output flow control

The native session starts with an output credit window. Unix only reads while
credit is available; Windows applies the same bound around its reader worker.
When native output is posted, the available credit is reduced by the exact
byte count. Dart acknowledges bytes when the output listener receives them.
Paused or not-yet-listened-to streams are held in a bounded Dart queue, and
cancelled or explicitly closed streams acknowledge and discard buffered data.

```text
PTY output --> native credit window --> event port --> Dart pending queue
                                                        |
                                      listener receives + acknowledges bytes
```

This keeps both the native read side and Dart-side pending output bounded by
the configured window. `OUTPUT_CLOSED` is independent of `PROCESS_EXIT`, so
trailing PTY bytes remain observable during the exit/drain interval.

## Input flow control

`PtyInput.write()` admits asynchronous buffers through the configured input
window, snapshots each admitted buffer, splits it into bounded chunks, and
submits chunks in order. Calls that would exceed the Dart-side admission window
wait for an earlier write to complete; callers should not mutate a buffer while
its write future is pending. Native queues copy each accepted chunk and report
completion by request ID. A full native queue produces `WRITABLE` after it
drains below its low-water mark, allowing Dart to resume without blocking the
isolate.

`tryWrite()` submits at most one native-sized request and never waits. An
accepted request remains tracked until `WRITE_COMPLETE` or an input/session
failure. A synchronous native write error fails pending input and requests
session shutdown. `flush()` waits for writes submitted before it was called,
including accepted `tryWrite()` requests.

```text
write(bytes) --> Dart bounded chunks --> native bounded queue --> PTY input
                                      ^             |
                                      +-- WRITABLE -+-- WRITE_COMPLETE
```

## Unix backend

Unix uses a nonblocking PTY master and a poll-based reactor. The reactor
monitors the PTY master and a nonblocking wake pipe, handles partial writes,
retries `EINTR`, and treats `EAGAIN` as backpressure. A separate waiter owns
`waitpid`; process exit wakes the reactor so it can preserve trailing output
before closing the PTY stream.

Spawn preparation resolves `PATH` before `fork()`, deep-copies all options,
and uses `execve()` with an explicit environment. A close-on-exec status pipe
communicates `setsid`, controlling-terminal, `dup2`, `chdir`, and `execve`
failures from the child without allocating after `fork()`.

PR CI also runs a source-level guard over that branch to reject allocator,
environment lookup, formatting, pthread, Dart API, and `execvp` calls from
being reintroduced there. The scheduled fork-safety test exercises the same
boundary while allocator-heavy sibling isolates are active.

Native-quality CI also spawns PTYs while allocator-heavy Dart sibling isolates
are active, guarding the fork boundary against inherited runtime state.

POSIX signals are sent to the configured process, process group, or foreground
process group. Descendants detached from that process group are outside the
guaranteed cleanup boundary.

## Windows backend

Windows uses ConPTY and a Job Object. Spawn creates and configures the Job
Object, builds a quoted UTF-16 command line and environment block, creates the
process suspended, assigns it to the Job Object, starts the native workers,
and then resumes it. The Job Object uses kill-on-close containment.

Dedicated reader, writer, waiter, and close workers keep blocking Win32 calls
off the Dart isolate. The writer handles partial `WriteFile` results and
reports request completion only after the complete chunk has been handed to
the PTY input channel. POSIX signals are unsupported on Windows rather than
being translated into a Job Object termination.

## Error and capability boundaries

Native failures are transported as domain, kind, OS code, and message fields.
Dart maps them to typed `PtyException` subclasses such as
`PtySpawnException`, `PtyIoException`, and `PtyUnsupportedException`.
Fatal asynchronous I/O errors fail the session futures, close input, and
request the idempotent native shutdown path so a failed worker cannot leave
the process running indefinitely.
The implementation also observes each internal lifecycle future so callers
that use only one of the public futures do not receive duplicate unhandled
errors; the original error remains available to every public future.
Malformed native event messages are treated as a fatal protocol error: Dart
fails the session futures, discards buffered output, closes input, and requests
native shutdown. The event pump remains alive until the native session reports
`SESSION_CLOSED`, so deterministic `close()` still completes normally. If a
malformed message carries the `SESSION_CLOSED` event tag, the pump also marks
the terminal wait complete because native shutdown has already finished.
Capabilities are reported by the backend, so callers can distinguish POSIX
signals, pixel dimensions, reliable process-tree cleanup, and ConPTY support
without inferring behavior from the host platform.

The clean-slate runtime is implemented for Linux, macOS, Windows, Android, and
iOS build targets. Linux and macOS integration are covered locally and in CI, and
the Android output/input subset has been verified on an API 35 emulator.
The iOS output/input subset has been verified on an iPhone simulator, but
physical-device runtime validation is still required before claiming
production iOS support. Windows runtime validation remains a release gate
until it has run on a Windows target.

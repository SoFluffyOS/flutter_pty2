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

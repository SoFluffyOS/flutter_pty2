import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:native_toolchain_c/native_toolchain_c.dart';

/// Compiles the native PTY library and bundles it as the code asset that
/// `lib/src/internal/native_symbols.dart` binds to. Mirrors
/// `src/CMakeLists.txt`, which builds the same library for the native tests.
void main(List<String> arguments) async {
  await build(arguments, (input, output) async {
    if (!input.config.buildCodeAssets) return;

    final targetOS = input.config.code.targetOS;
    final platformSources = switch (targetOS) {
      OS.windows => const [
        'src/windows/pty_windows_commandline.c',
        'src/windows/pty_windows_environment.c',
        'src/windows/pty_windows_spawn.c',
        'src/windows/pty_windows_session.c',
      ],
      _ => const ['src/unix/pty_unix_spawn.c', 'src/unix/pty_unix_session.c'],
    };

    await CBuilder.library(
      name: 'flutter_pty2',
      assetName: 'flutter_pty2',
      sources: [
        'src/include/dart_api_dl.c',
        'src/common/pty_error.c',
        'src/common/pty_event.c',
        'src/common/pty_refcount.c',
        'src/common/pty_debug_stats.c',
        'src/common/pty_write_queue.c',
        ...platformSources,
      ],
      includes: const ['src', 'src/include'],
      defines: const {'DART_SHARED_LIB': null},
      libraries: switch (targetOS) {
        OS.linux => const ['util', 'pthread'],
        _ => const [],
      },
      flags: switch (targetOS) {
        // Supports Android 15 devices with 16 KB pages.
        OS.android => const ['-Wl,-z,max-page-size=16384'],
        _ => const [],
      },
      std: 'c11',
    ).run(input: input, output: output);
  });
}

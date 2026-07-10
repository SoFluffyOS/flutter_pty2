import 'package:flutter_pty/src/environment.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('Windows environment overrides keys case-insensitively', () {
    final environment = buildPtyEnvironment(
      {'Path': 'base', 'HOME': 'home'},
      {'PATH': 'override'},
      caseInsensitive: true,
    );

    expect(environment['Path'], isNull);
    expect(environment['PATH'], 'override');
    expect(environment['TERM'], 'xterm-256color');
    expect(environment['COLORTERM'], 'truecolor');
    expect(environment['TERM_PROGRAM'], 'Lumide');
  });

  test('Windows environment block entries are case-insensitively sorted', () {
    final entries = orderPtyEnvironment(
      {'z': 'last', 'Beta': 'middle', 'alpha': 'first'},
      caseInsensitive: true,
    );

    expect(entries.map((entry) => entry.key), ['alpha', 'Beta', 'z']);
  });

  test('PTY environment strips inherited desktop startup tokens', () {
    final environment = buildPtyEnvironment(
      {
        'DESKTOP_STARTUP_ID': 'activation',
        'XDG_ACTIVATION_TOKEN': 'token',
        'HOME': 'home',
      },
      null,
      caseInsensitive: false,
    );

    expect(environment['DESKTOP_STARTUP_ID'], isNull);
    expect(environment['XDG_ACTIVATION_TOKEN'], isNull);
    expect(environment['HOME'], 'home');
  });

  test('Windows PTY environment strips startup tokens case-insensitively', () {
    final environment = buildPtyEnvironment(
      {
        'desktop_startup_id': 'activation',
        'xdg_activation_token': 'token',
        'HOME': 'home',
      },
      null,
      caseInsensitive: true,
    );

    expect(environment['desktop_startup_id'], isNull);
    expect(environment['xdg_activation_token'], isNull);
    expect(environment['HOME'], 'home');
  });

  test('PTY environment strips inherited terminal identity', () {
    final environment = buildPtyEnvironment(
      {
        'TERM_PROGRAM': 'Apple_Terminal',
        'TERM_PROGRAM_VERSION': '999',
        'VTE_VERSION': '7600',
      },
      null,
      caseInsensitive: false,
    );

    expect(environment['TERM_PROGRAM'], 'Lumide');
    expect(environment['TERM_PROGRAM_VERSION'], isNull);
    expect(environment['VTE_VERSION'], isNull);
  });

  test('Windows PTY environment strips terminal identity case-insensitively',
      () {
    final environment = buildPtyEnvironment(
      {
        'term_program_version': '999',
        'vte_version': '7600',
      },
      null,
      caseInsensitive: true,
    );

    expect(environment['term_program_version'], isNull);
    expect(environment['vte_version'], isNull);
    expect(environment['TERM_PROGRAM'], 'Lumide');
  });

  test('Unix environment preserves case-distinct keys and insertion order', () {
    final environment = buildPtyEnvironment(
      {'Path': 'base'},
      {'PATH': 'override'},
      caseInsensitive: false,
    );
    final entries = orderPtyEnvironment(
      environment,
      caseInsensitive: false,
    );

    expect(environment['Path'], 'base');
    expect(environment['PATH'], 'override');
    expect(entries.first.key, 'Path');
  });
}

import 'package:flutter_pty2/src/pty_environment.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('case-insensitive replacement collapses duplicate keys', () {
    final environment = buildEnvironment(
      const PtyEnvironment.replace({
        'Path': 'first',
        'PATH': 'second',
      }),
      caseInsensitive: true,
    );

    expect(environment.keys, ['PATH']);
    expect(environment['PATH'], 'second');
  });

  test('inherited environment applies normalized removals and overrides', () {
    final environment = buildEnvironment(
      const PtyEnvironment.inherit(
        overrides: {'PtyFlutter2TestValue': 'override'},
        remove: {'PATH'},
      ),
      caseInsensitive: true,
    );

    expect(environment['PTYFLUTTER2TESTVALUE'], 'override');
    expect(environment['PATH'], isNull);
  });

  test('case-sensitive replacement preserves caller keys', () {
    final environment = buildEnvironment(
      const PtyEnvironment.replace({
        'Path': 'first',
        'PATH': 'second',
      }),
      caseInsensitive: false,
    );

    expect(environment['Path'], 'first');
    expect(environment['PATH'], 'second');
  });

  test('environment validation rejects malformed keys and values', () {
    expect(
      () => buildEnvironment(
        const PtyEnvironment.replace({'BAD=KEY': 'value'}),
        caseInsensitive: false,
      ),
      throwsArgumentError,
    );
    expect(
      () => buildEnvironment(
        const PtyEnvironment.replace({'KEY': 'bad\x00value'}),
        caseInsensitive: false,
      ),
      throwsArgumentError,
    );
  });
}

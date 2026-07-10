void validatePtyStartOptions({
  required String executable,
  required List<String> arguments,
  required String? workingDirectory,
  required Map<String, String>? environment,
  required int rows,
  required int columns,
}) {
  _validateString('executable', executable);
  if (executable.isEmpty) {
    throw ArgumentError.value(executable, 'executable', 'must not be empty');
  }

  for (var i = 0; i < arguments.length; i++) {
    _validateString('arguments[$i]', arguments[i]);
  }

  if (workingDirectory case final directory?) {
    _validateString('workingDirectory', directory);
  }

  if (environment case final values?) {
    for (final entry in values.entries) {
      _validateString('environment key', entry.key);
      _validateString('environment value for ${entry.key}', entry.value);
      if (entry.key.isEmpty) {
        throw ArgumentError.value(
            entry.key, 'environment key', 'must not be empty');
      }
      if (entry.key.contains('=')) {
        throw ArgumentError.value(
          entry.key,
          'environment key',
          'must not contain =',
        );
      }
    }
  }

  validatePtySize(rows: rows, columns: columns);
}

void validatePtySize({
  required int rows,
  required int columns,
  int pixelWidth = 0,
  int pixelHeight = 0,
}) {
  _validateDimension('rows', rows, 1, 0x7fff);
  _validateDimension('columns', columns, 1, 0x7fff);
  _validateDimension('pixelWidth', pixelWidth, 0, 0xffff);
  _validateDimension('pixelHeight', pixelHeight, 0, 0xffff);
}

void _validateString(String name, String value) {
  if (value.contains('\x00')) {
    throw ArgumentError.value(value, name, 'must not contain NUL bytes');
  }
}

void _validateDimension(String name, int value, int minimum, int maximum) {
  if (value < minimum || value > maximum) {
    throw ArgumentError.value(
      value,
      name,
      'must be between $minimum and $maximum',
    );
  }
}

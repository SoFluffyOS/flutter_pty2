const _desktopStartupEnvironmentKeys = {
  'DESKTOP_STARTUP_ID',
  'XDG_ACTIVATION_TOKEN',
};

const _inheritedTerminalIdentityKeys = {
  'TERM_PROGRAM_VERSION',
  'VTE_VERSION',
};

Map<String, String> buildPtyEnvironment(
  Map<String, String> base,
  Map<String, String>? overrides, {
  required bool caseInsensitive,
}) {
  final result = <String, String>{
    ...base,
    'TERM': 'xterm-256color',
    'COLORTERM': 'truecolor',
    'TERM_PROGRAM': 'Lumide',
  };
  _removeDesktopStartupEnvironment(result, caseInsensitive: caseInsensitive);
  _removeInheritedTerminalIdentity(result, caseInsensitive: caseInsensitive);
  if (overrides == null) {
    return result;
  }

  for (final entry in overrides.entries) {
    if (caseInsensitive) {
      final normalizedKey = entry.key.toLowerCase();
      result.removeWhere((key, _) => key.toLowerCase() == normalizedKey);
    }
    result[entry.key] = entry.value;
  }
  return result;
}

void _removeDesktopStartupEnvironment(
  Map<String, String> environment, {
  required bool caseInsensitive,
}) {
  for (final key in _desktopStartupEnvironmentKeys) {
    if (!caseInsensitive) {
      environment.remove(key);
      continue;
    }

    final normalizedKey = key.toLowerCase();
    environment.removeWhere(
      (environmentKey, _) => environmentKey.toLowerCase() == normalizedKey,
    );
  }
}

void _removeInheritedTerminalIdentity(
  Map<String, String> environment, {
  required bool caseInsensitive,
}) {
  for (final key in _inheritedTerminalIdentityKeys) {
    if (!caseInsensitive) {
      environment.remove(key);
      continue;
    }

    final normalizedKey = key.toLowerCase();
    environment.removeWhere(
      (environmentKey, _) => environmentKey.toLowerCase() == normalizedKey,
    );
  }
}

List<MapEntry<String, String>> orderPtyEnvironment(
  Map<String, String> environment, {
  required bool caseInsensitive,
}) {
  final entries = environment.entries.toList(growable: false);
  if (!caseInsensitive) {
    return entries;
  }
  entries.sort(
    (first, second) =>
        first.key.toLowerCase().compareTo(second.key.toLowerCase()),
  );
  return entries;
}

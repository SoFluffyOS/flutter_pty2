const _desktopStartupEnvironmentKeys = {
  'DESKTOP_STARTUP_ID',
  'XDG_ACTIVATION_TOKEN',
};

const _inheritedTerminalIdentityKeys = {
  'ALACRITTY_LOG',
  'ALACRITTY_WINDOW_ID',
  'KONSOLE_VERSION',
  'TERM_SESSION_ID',
  'TERM_PROGRAM_VERSION',
  'VTE_VERSION',
  'WT_PROFILE_ID',
  'WT_SESSION',
};

const _inheritedTerminalIdentityPrefixes = {
  'GHOSTTY_',
  'KITTY_',
  'WEZTERM_',
};

const _localeEnvironmentKeys = {
  'LANG',
  'LC_ALL',
  'LC_CTYPE',
};

Map<String, String> buildPtyEnvironment(
  Map<String, String> base,
  Map<String, String>? overrides, {
  required bool caseInsensitive,
  String? terminalProgramVersion,
}) {
  final result = <String, String>{
    ...base,
    'TERM': 'xterm-256color',
    'COLORTERM': 'truecolor',
    'TERM_PROGRAM': 'Lumide',
  };
  _removeDesktopStartupEnvironment(result, caseInsensitive: caseInsensitive);
  _removeInheritedTerminalIdentity(result, caseInsensitive: caseInsensitive);
  _ensureUtf8Locale(result, caseInsensitive: caseInsensitive);
  if (terminalProgramVersion != null && terminalProgramVersion.isNotEmpty) {
    result['TERM_PROGRAM_VERSION'] = terminalProgramVersion;
  }
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

void _ensureUtf8Locale(
  Map<String, String> environment, {
  required bool caseInsensitive,
}) {
  if (_hasLocaleEnvironment(environment, caseInsensitive: caseInsensitive)) {
    return;
  }

  environment['LANG'] = 'en_US.UTF-8';
}

bool _hasLocaleEnvironment(
  Map<String, String> environment, {
  required bool caseInsensitive,
}) {
  for (final key in _localeEnvironmentKeys) {
    if (!caseInsensitive) {
      final value = environment[key];
      if (value != null && value.isNotEmpty) {
        return true;
      }
      continue;
    }

    final normalizedKey = key.toLowerCase();
    for (final entry in environment.entries) {
      if (entry.key.toLowerCase() == normalizedKey && entry.value.isNotEmpty) {
        return true;
      }
    }
  }
  return false;
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

  for (final prefix in _inheritedTerminalIdentityPrefixes) {
    if (!caseInsensitive) {
      environment.removeWhere(
        (environmentKey, _) => environmentKey.startsWith(prefix),
      );
      continue;
    }

    final normalizedPrefix = prefix.toLowerCase();
    environment.removeWhere(
      (environmentKey, _) =>
          environmentKey.toLowerCase().startsWith(normalizedPrefix),
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

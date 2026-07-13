const _desktopStartupEnvironmentKeys = {
  'DESKTOP_STARTUP_ID',
  'XDG_ACTIVATION_TOKEN',
};

const _inheritedTerminalIdentityKeys = {
  'ALACRITTY_LOG',
  'ALACRITTY_WINDOW_ID',
  'KONSOLE_VERSION',
  'TERM_SESSION_ID',
  'TERM_PROGRAM',
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
  if (overrides != null) {
    for (final entry in overrides.entries) {
      if (caseInsensitive) {
        final normalizedKey = entry.key.toLowerCase();
        result.removeWhere((key, _) => key.toLowerCase() == normalizedKey);
      }
      result[entry.key] = entry.value;
    }
  }
  _removeDesktopStartupEnvironment(result, caseInsensitive: caseInsensitive);
  _removeInheritedTerminalIdentity(result, caseInsensitive: caseInsensitive);
  _ensureUtf8Locale(result, caseInsensitive: caseInsensitive);
  result['TERM_PROGRAM'] = 'Lumide';
  if (terminalProgramVersion != null && terminalProgramVersion.isNotEmpty) {
    _setCanonicalEnvironmentValue(
      result,
      'TERM_PROGRAM_VERSION',
      terminalProgramVersion,
      caseInsensitive: caseInsensitive,
    );
  }
  return result;
}

void _ensureUtf8Locale(
  Map<String, String> environment, {
  required bool caseInsensitive,
}) {
  final lcAll = _environmentValue(
    environment,
    key: 'LC_ALL',
    caseInsensitive: caseInsensitive,
  );
  if (lcAll != null && lcAll.isNotEmpty) {
    return;
  }

  final lang = _environmentValue(
    environment,
    key: 'LANG',
    caseInsensitive: caseInsensitive,
  );
  if (!_isUtf8Locale(lang)) {
    _setCanonicalEnvironmentValue(
      environment,
      'LANG',
      'en_US.UTF-8',
      caseInsensitive: caseInsensitive,
    );
  }

  final lcCtype = _environmentValue(
    environment,
    key: 'LC_CTYPE',
    caseInsensitive: caseInsensitive,
  );
  if (!_isUtf8Locale(lcCtype)) {
    _setCanonicalEnvironmentValue(
      environment,
      'LC_CTYPE',
      'en_US.UTF-8',
      caseInsensitive: caseInsensitive,
    );
  }
}

String? _environmentValue(
  Map<String, String> environment, {
  required String key,
  required bool caseInsensitive,
}) {
  if (!caseInsensitive) {
    return environment[key];
  }

  final normalizedKey = key.toLowerCase();
  for (final entry in environment.entries) {
    if (entry.key.toLowerCase() == normalizedKey) {
      return entry.value;
    }
  }
  return null;
}

bool _isUtf8Locale(String? locale) {
  if (locale == null || locale.isEmpty) {
    return false;
  }

  final normalizedLocale = locale.toLowerCase();
  return normalizedLocale.contains('utf-8') ||
      normalizedLocale.contains('utf8');
}

void _setCanonicalEnvironmentValue(
  Map<String, String> environment,
  String key,
  String value, {
  required bool caseInsensitive,
}) {
  if (caseInsensitive) {
    final normalizedKey = key.toLowerCase();
    environment.removeWhere(
      (environmentKey, _) => environmentKey.toLowerCase() == normalizedKey,
    );
  }
  environment[key] = value;
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

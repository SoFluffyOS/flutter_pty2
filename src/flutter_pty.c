#include "flutter_pty_legacy.h"

#include "include/dart_api_dl.c"

#if _WIN32
#include "flutter_pty_win.c"
#include "windows/pty_windows_session.c"
#else
#include "forkpty.c"
#include "flutter_pty_unix.c"
#include "unix/pty_unix_spawn.c"
#include "unix/pty_unix_session.c"
#endif

#include "flutter_pty_legacy.h"

#include "include/dart_api_dl.c"

#if _WIN32
#include "flutter_pty_win.c"
#include "windows/pty_windows_spawn.c"
#include "windows/pty_windows_session.c"
#else
#include "forkpty.c"
#include "flutter_pty_unix.c"
#include "unix/pty_unix_spawn.c"
#include "unix/pty_unix_session.c"
#endif

#if defined(FLUTTER_PTY2_INCLUDE_COMMON_SOURCES)
#include "common/pty_debug_stats.c"
#include "common/pty_error.c"
#include "common/pty_event.c"
#include "common/pty_refcount.c"
#include "common/pty_write_queue.c"
#endif

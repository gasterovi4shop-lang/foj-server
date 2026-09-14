#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdbool.h>

// spawns the stdin command reader (console mode only)
bool console_start(void);

// text of the command list (used by .help and the admin API)
void console_help_text(char* out, size_t cap);

// executes a console command line on server `srv_index`. Output goes to
// stdout when `out` is NULL, otherwise into the caller's buffer.
// `src` tags the caller in the command log ("console", "bot", ...).
// Used by the stdin console and by the admin TCP API (Admin.c).
bool console_exec_ctx(int srv_index, char* line, const char* src, char* out, size_t outcap);

// one executed command, newest first in console_cmdlog_get()
typedef struct
{
	char time[24];
	char src[12];
	char line[128];
} ConsoleCmdLog;

#define CMDLOG_CAP 64

// copies up to `cap` recent commands into `out`, newest first; returns count
int console_cmdlog_get(ConsoleCmdLog* out, int cap);

// internal: creates the exec mutex once (console mode may not be running)
bool console_lock_init(void);

#endif

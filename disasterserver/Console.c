// Командная консоль для режима без окна (--nogui или обычная сборка без UI).
// Позволяет пользоваться функционалом панели прямо из консоли: баны с
// длительностью и причиной, кик, отправка новостей, список игроков.
// Тот же набор команд доступен и внешним клиентам через админ-API (Admin.c):
// console_exec_ctx() исполняет строку и отдаёт вывод в буфер.
#include <Console.h>
#include <Config.h>
#include <Lib.h>
#include <Log.h>
#include <Server.h>
#include <States.h>
#include <Maps.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <io/Threads.h>

static Server* console_server(void)
{
	return disaster_get(0);
}

void console_help_text(char* out, size_t cap)
{
	snprintf(out, cap,
		".ban <nick> [time] [reason] - ban (30m/12h/7d/2w/1d12h/perm)\n"
		".kick <nick> [reason] - kick a player\n"
		".banid <id> [time] [reason] - ban by account id (works offline)\n"
		".unbanid <id> - remove a ban by account id\n"
		".kickid <id> [reason] - kick by account id\n"
		".opid <id> - grant operator rights by account id\n"
		".myid - show your system account id\n"
		".set_id <id> - set your custom id (3-18 letters, digits, _ or -)\n"
		".reset_id [system_id] - reset your custom id; operators may specify another id\n"
		".news <text> - send a newsletter notification\n"
		".list - online players\n"
		".map <1-%d> - force a map (starts char select)\n"
		".forceescape - force survivors win in the running match\n"
		".forceexe - force exe win in the running match\n"
		".return - return the server to the lobby\n"
		"exit - stop the server", MAP_COUNT + 1);
}

// ---------------------------------------------------------------- command log
// последние исполненные команды - для админ-API (бот тянет их в свой журнал)
#define CMDLOG_CAP 64

typedef struct
{
	char time[24];
	char src[12];
	char line[128];
} CmdLogEntry;

static CmdLogEntry g_cmdlog[CMDLOG_CAP];
static int g_cmdlog_head;	// next write slot
static bool g_cmdlog_full;

static void cmdlog_record(const char* src, const char* line)
{
	CmdLogEntry* e = &g_cmdlog[g_cmdlog_head];

	time_t now = time(NULL);
	strftime(e->time, sizeof(e->time), "%d.%m %H:%M:%S", localtime(&now));
	snprintf(e->src, sizeof(e->src), "%s", src);
	snprintf(e->line, sizeof(e->line), "%s", line);

	g_cmdlog_head = (g_cmdlog_head + 1) % CMDLOG_CAP;
	if (g_cmdlog_head == 0)
		g_cmdlog_full = true;
}

int console_cmdlog_get(ConsoleCmdLog* out, int cap)
{
	if (!out || cap <= 0)
		return 0;

	int total = g_cmdlog_full ? CMDLOG_CAP : g_cmdlog_head;
	int n = (total < cap) ? total : cap;

	// newest first
	for (int i = 0; i < n; i++)
	{
		int idx = (g_cmdlog_head - 1 - i + CMDLOG_CAP * 2) % CMDLOG_CAP;
		CmdLogEntry* e = &g_cmdlog[idx];
		ConsoleCmdLog* o = &out[i];
		snprintf(o->time, sizeof(o->time), "%s", e->time);
		snprintf(o->src, sizeof(o->src), "%s", e->src);
		snprintf(o->line, sizeof(o->line), "%s", e->line);
	}

	return n;
}

// Вывод команды: в консоль через printf, либо в буфер вызывающего
// (когда команду исполняет админ-API). Под одним мьютексом с exec.
static Mutex	g_execMut;
static char*	g_outbuf;
static size_t	g_outcap;
static size_t	g_outlen;

static bool exec_lock(void)
{
	MutexLock(g_execMut);
	return true;
}

static void out_reset(char* buf, size_t cap)
{
	g_outbuf = buf;
	g_outcap = cap;
	g_outlen = 0;
	if (buf && cap)
		buf[0] = '\0';
}

static void out(const char* fmt, ...)
{
	char line[1024];

	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	if (g_outbuf)
	{
		size_t len = strlen(line);
		if (g_outlen + len + 1 > g_outcap)
			len = g_outcap - g_outlen - 1;

		memcpy(g_outbuf + g_outlen, line, len);
		g_outlen += len;
		g_outbuf[g_outlen] = '\0';
	}
	else
		printf("%s\n", line);
}

// MutexLock внутри развернётся в "return false" - поэтому функция bool,
// а не void (иначе MSVC C4098)
static bool console_exec(Server* server, char* line)
{
	// trim
	while (*line == ' ' || *line == '\t')
		line++;

	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
		line[--len] = '\0';

	if (line[0] == '\0')
		return true;

	if (!server)
	{
		out("no server is running");
		return true;
	}

	char* cmd = strtok(line, " ");

	if (strcmp(cmd, ".ban") == 0)
	{
		char* nick = strtok(NULL, " ");
		char* dur = strtok(NULL, " ");
		char* reason = dur ? strtok(NULL, "") : NULL;

		if (!nick)
		{
			out("usage: .ban <nick> [time] [reason]");
			return true;
		}

		char confirm[256];
		server_host_ban(server, nick, dur ? dur : "perm", reason ? reason : "", confirm, sizeof(confirm));
		out("%s", confirm);
		return true;
	}

	if (strcmp(cmd, ".kick") == 0)
	{
		char* nick = strtok(NULL, " ");
		char* reason = nick ? strtok(NULL, "") : NULL;

		if (!nick)
		{
			out("usage: .kick <nick> [reason]");
			return true;
		}

		char confirm[256];
		server_host_kick(server, nick, reason ? reason : "", confirm, sizeof(confirm));
		out("%s", confirm);
		return true;
	}

	if (strcmp(cmd, ".banid") == 0)
	{
		char* id = strtok(NULL, " ");
		char* dur = strtok(NULL, " ");
		char* reason = dur ? strtok(NULL, "") : NULL;

		if (!id)
		{
			out("usage: .banid <id> <time> [reason]");
			return true;
		}

		char confirm[256];
		server_host_ban_udid(id, dur ? dur : "perm", reason ? reason : "", confirm, sizeof(confirm));
		out("%s", confirm);
		return true;
	}

	if (strcmp(cmd, ".unbanid") == 0)
	{
		char* id = strtok(NULL, " ");

		if (!id)
		{
			out("usage: .unbanid <id>");
			return true;
		}

		char confirm[256];
		server_host_unban(id, confirm, sizeof(confirm));
		out("%s", confirm);
		return true;
	}

	if (strcmp(cmd, ".kickid") == 0)
	{
		char* id = strtok(NULL, " ");
		char* reason = id ? strtok(NULL, "") : NULL;

		if (!id)
		{
			out("usage: .kickid <id> [reason]");
			return true;
		}

		char confirm[256];
		server_host_kick_id(id, reason ? reason : "", confirm, sizeof(confirm));
		out("%s", confirm);
		return true;
	}

	if (strcmp(cmd, ".opid") == 0)
	{
		char* id = strtok(NULL, " ");

		if (!id)
		{
			out("usage: .opid <id>");
			return true;
		}

		char confirm[256];
		server_host_op_id(id, confirm, sizeof(confirm));
		out("%s", confirm);
		return true;
	}

	if (strcmp(cmd, ".map") == 0)
	{
		char* arg = strtok(NULL, " ");
		int ind = arg ? atoi(arg) : 0;

		if (ind < 1 || ind > MAP_COUNT + 1)
		{
			out("usage: .map <1-%d>", MAP_COUNT + 1);
			return true;
		}

		if (!charselect_init(ind - 1, server))
			lobby_init(server);
		out("map %d selected", ind);
		return true;
	}

	if (strcmp(cmd, ".forceescape") == 0 || strcmp(cmd, ".forceexe") == 0)
	{
		if (server->state != ST_GAME)
		{
			out("the match is not running right now");
			return true;
		}

		game_end(server, (strcmp(cmd, ".forceescape") == 0) ? ED_SURVWIN : ED_EXEWIN, false);
		out("match result forced: %s", (strcmp(cmd, ".forceescape") == 0) ? "survivors win" : "exe win");
		return true;
	}

	if (strcmp(cmd, ".return") == 0)
	{
		if (server->state == ST_LOBBY)
		{
			out("already in the lobby");
			return true;
		}

		results_init(server);
		out("returning to the lobby");
		return true;
	}

	if (strcmp(cmd, ".news") == 0)
	{
		char* text = strtok(NULL, "");

		if (!text || text[0] == '\0')
		{
			out("usage: .news <text> (\\n makes a line break)");
			return true;
		}

		if (news_push(text))
			out("news sent");
		else
			out("failed to save news");

		return true;
	}

	if (strcmp(cmd, ".list") == 0)
	{
		MutexLock(server->state_lock);
		{
			int n = 0;
			for (size_t i = 0; i < server->peers.capacity; i++)
			{
				PeerData* peer = (PeerData*)server->peers.ptr[i];
				if (!peer)
					continue;

				out("%u: %s", peer->id, peer->nickname.value);
				n++;
			}

			if (n == 0)
				out("nobody is online");
		}
		MutexUnlock(server->state_lock);
		return true;
	}

	if (strcmp(cmd, ".help") == 0 || strcmp(cmd, "help") == 0)
	{
		char buf[1024];
		console_help_text(buf, sizeof(buf));

		// печатаем построчно (в буфере разбиение на строки тоже сохранится)
		char* p = buf;
		while (*p)
		{
			char* nl = strchr(p, '\n');
			if (nl)
				*nl = '\0';
			out("%s", p);
			if (!nl)
				break;
			p = nl + 1;
		}

		return true;
	}

	if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0)
	{
		out("shutting down...");
		disaster_shutdown();
		return true;
	}

	out("unknown command '%s', type .help", cmd);
	return true;
}

// Исполняет строку команды от консоли ("console"), админ-API ("bot") или
// другого источника; вывод идёт в stdout (out == NULL) или в буфер.
// Каждая команда попадает в журнал команд (console_cmdlog_get).
bool console_exec_ctx(int srv_index, char* line, const char* src, char* out, size_t outcap)
{
	console_lock_init();
	Server* server = disaster_get(srv_index);

	if (!exec_lock())
		return false;

	// console_exec парсит строку через strtok и портит её - для журнала нужен оригинал
	char original[256];
	snprintf(original, sizeof(original), "%s", line);

	out_reset(out, outcap);
	bool ok = console_exec(server, line);

	cmdlog_record(src ? src : "unknown", original);

	g_outbuf = NULL;
	MutexUnlock(g_execMut);
	return ok;
}

static void console_thread(void* arg)
{
	(void)arg;
	char line[1024];

	printf("console: type .help for commands\n");

	for (;;)
	{
		if (fgets(line, sizeof(line), stdin) == NULL)
			break;

		console_exec_ctx(0, line, "console", NULL, 0);
	}
}

// мьютекс нужен и без консольного режима (Admin.c исполняет те же команды)
bool console_lock_init(void)
{
	static bool created = false;
	if (created)
		return true;

	created = true;
	MutexCreate(g_execMut);
	return true;
}

bool console_start(void)
{
	console_lock_init();

	Thread th;
	ThreadSpawn(th, console_thread, NULL);
	return true;
}

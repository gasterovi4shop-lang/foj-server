#include <Admin.h>
#include <Config.h>
#include <Console.h>
#include <Lib.h>
#include <Log.h>
#include <Server.h>
#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <unistd.h>
#endif

// Слушатель админ-команд: тот же принцип, что у "новостника" (News.c).
// Клиент (телеграм-бот) шлёт одну json-строку и получает одну json-строку.
#define ADMIN_MAX_LINE	4096
#define EVENTLOG_CAP 512

typedef struct
{
	char time[24];
	char nick[32];
	char accid[16];
	char text[128];
	int server;
	int player_id;
	bool command;
} ChatLogEntry;

static ChatLogEntry g_events[EVENTLOG_CAP];
static int g_events_head;
static bool g_events_full;
static Mutex g_eventMut;

static bool event_is_command(const char* text)
{
	while (text && (*text == ' ' || *text == '\t'))
		text++;
	return text && text[0] == '.';
}

// MutexLock внутри развернётся в "return false" - поэтому функции bool, а не void
static bool status_lock(Server* server)
{
	MutexLock(server->state_lock);
	return true;
}

SERVER_API void admin_log_chat(int server, const char* nick, const char* accid,
	int player_id, const char* text)
{
	if (!text)
		return;

	MutexLock(g_eventMut);
	{
		ChatLogEntry* e = &g_events[g_events_head];
		time_t now = time(NULL);
		strftime(e->time, sizeof(e->time), "%Y-%m-%dT%H:%M:%S", localtime(&now));
		snprintf(e->nick, sizeof(e->nick), "%s", nick ? nick : "");
		snprintf(e->accid, sizeof(e->accid), "%s", accid ? accid : "");
		snprintf(e->text, sizeof(e->text), "%s", text);
		e->server = server;
		e->player_id = player_id;
		e->command = event_is_command(text);
		g_events_head = (g_events_head + 1) % EVENTLOG_CAP;
		if (g_events_head == 0)
			g_events_full = true;
	}
	MutexUnlock(g_eventMut);
}

static const char* state_name(int state)
{
	switch (state)
	{
		case ST_LOBBY:		return "lobby";
		case ST_MAPVOTE:	return "mapvote";
		case ST_CHARSELECT:	return "charselect";
		case ST_GAME:		return "game";
		case ST_RESULTS:	return "results";
	}

	return "unknown";
}

static cJSON* handle_status(void)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "servers");

	for (int i = 0; i < disaster_count(); i++)
	{
		Server* server = disaster_get(i);
		if (!server)
			continue;

		int total = 0;
		int ingame = 0;

		status_lock(server);
		{
			for (size_t p = 0; p < server->peers.capacity; p++)
			{
				PeerData* peer = (PeerData*)server->peers.ptr[p];
				if (!peer)
					continue;

				total++;
				if (peer->in_game)
					ingame++;
			}
		}
		MutexUnlock(server->state_lock);

		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "id", i);
		cJSON_AddNumberToObject(item, "online", total);
		cJSON_AddNumberToObject(item, "ingame", ingame);
		cJSON_AddStringToObject(item, "state", state_name(server->state));
		cJSON_AddItemToArray(arr, item);
	}

	return resp;
}

static cJSON* handle_chat_lobbies(void)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "lobbies");

	for (int i = 0; i < disaster_count(); i++)
	{
		Server* server = disaster_get(i);
		if (!server)
			continue;

		int online = 0;
		status_lock(server);
		{
			for (size_t p = 0; p < server->peers.capacity; p++)
				if (server->peers.ptr[p])
					online++;
		}
		MutexUnlock(server->state_lock);

		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "id", i);
		cJSON_AddNumberToObject(item, "online", online);
		cJSON_AddStringToObject(item, "state", state_name(server->state));
		cJSON_AddItemToArray(arr, item);
	}

	return resp;
}

static cJSON* handle_players(int srv_index)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "players");

	Server* server = disaster_get(srv_index);
	if (!server)
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "no such server");
		return resp;
	}

	status_lock(server);
	{
		for (size_t p = 0; p < server->peers.capacity; p++)
		{
			PeerData* peer = (PeerData*)server->peers.ptr[p];
			if (!peer)
				continue;

			cJSON* item = cJSON_CreateObject();
			cJSON_AddNumberToObject(item, "id", peer->id);
			cJSON_AddStringToObject(item, "nick", peer->nickname.value);
			cJSON_AddStringToObject(item, "accid", peer->accid);
			cJSON_AddStringToObject(item, "custom_id",
				peer->custom_id[0] && strcmp(peer->custom_id, peer->accid) != 0
					? peer->custom_id : "");
			cJSON_AddStringToObject(item, "udid", peer->udid.value);
			cJSON_AddStringToObject(item, "ip", peer->ip.value);
			cJSON_AddBoolToObject(item, "in_game", peer->in_game);
			cJSON_AddBoolToObject(item, "spectating", peer->spectating);
			cJSON_AddBoolToObject(item, "op", peer->op);
			cJSON_AddItemToArray(arr, item);
		}
	}
	MutexUnlock(server->state_lock);

	return resp;
}

// ---------------------------------------------------------------- join log
#define JOINLOG_CAP 100

typedef struct
{
	char time[24];
	char nick[32];
	char accid[16];
	char udid[48];
	int id;
} JoinLogEntry;

static JoinLogEntry g_joins[JOINLOG_CAP];
static int g_joins_head;
static bool g_joins_full;
static Mutex g_joinMut;

// MutexLock разворачивается в "return false" - поэтому функции bool
static bool join_lock(void)
{
	MutexLock(g_joinMut);
	return true;
}

static bool join_unlock(void)
{
	MutexUnlock(g_joinMut);
	return true;
}

// ---------------------------------------------------------------- known ids
// реестр выданных ID + игровое время: персистится в known_ids.json
typedef struct
{
	char accid[16];
	char nick[32];
	double seconds;
	uint64_t first_seen;
} KnownId;

#define KNOWN_CAP 1024
#define KNOWN_FILE "known_ids.json"

static KnownId g_known[KNOWN_CAP];
static int g_known_n;
static Mutex g_knownMut;
static double g_known_last_save;

static bool known_lock(void)
{
	MutexLock(g_knownMut);
	return true;
}

static bool known_unlock(void)
{
	MutexUnlock(g_knownMut);
	return true;
}

static KnownId* known_find(const char* accid)
{
	for (int i = 0; i < g_known_n; i++)
		if (strcmp(g_known[i].accid, accid) == 0)
			return &g_known[i];
	return NULL;
}

static void known_save(void)
{
	cJSON* root = cJSON_CreateObject();
	for (int i = 0; i < g_known_n; i++)
	{
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "nick", g_known[i].nick);
		cJSON_AddNumberToObject(item, "seconds", g_known[i].seconds);
		cJSON_AddNumberToObject(item, "first_seen", (double)g_known[i].first_seen);
		cJSON_AddItemToObject(root, g_known[i].accid, item);
	}
	collection_save(KNOWN_FILE, root);
	cJSON_Delete(root);
}

static KnownId* known_upsert(const char* accid, const char* nick)
{
	KnownId* e = known_find(accid);
	if (!e)
	{
		if (g_known_n >= KNOWN_CAP)
			return NULL;

		e = &g_known[g_known_n++];
		memset(e, 0, sizeof(*e));
		snprintf(e->accid, sizeof(e->accid), "%s", accid);
		e->first_seen = (uint64_t)time(NULL);
	}

	if (nick && nick[0] && strcmp(e->nick, nick) != 0)
		snprintf(e->nick, sizeof(e->nick), "%s", nick);

	return e;
}

SERVER_API void admin_log_join(const char* nick, const char* accid, const char* udid, int id)
{
	join_lock();
	{
		JoinLogEntry* e = &g_joins[g_joins_head];

		time_t now = time(NULL);
		strftime(e->time, sizeof(e->time), "%d.%m %H:%M:%S", localtime(&now));
		snprintf(e->nick, sizeof(e->nick), "%s", nick ? nick : "?");
		snprintf(e->accid, sizeof(e->accid), "%s", accid ? accid : "");
		snprintf(e->udid, sizeof(e->udid), "%s", udid ? udid : "");
		e->id = id;

		g_joins_head = (g_joins_head + 1) % JOINLOG_CAP;
		if (g_joins_head == 0)
			g_joins_full = true;
	}
	join_unlock();

	// регистрируем выданный ID (если новый - сохраняем реестр)
	if (accid && accid[0])
	{
		known_lock();
		{
			int was_new = (known_find(accid) == NULL);
			known_upsert(accid, nick);
			if (was_new)
				known_save();
		}
		known_unlock();
	}
}

SERVER_API void admin_add_playtime(const char* accid, const char* nick, double seconds)
{
	if (!accid || accid[0] == '\0' || seconds <= 0)
		return;

	known_lock();
	{
		KnownId* e = known_upsert(accid, nick);
		if (e)
			e->seconds += seconds;

		// периодическое сохранение игрового времени
		time_t now = time(NULL);
		if ((double)now - g_known_last_save > 300)
		{
			g_known_last_save = (double)now;
			known_save();
		}
	}
	known_unlock();
}

SERVER_API bool admin_profile(const char* accid, char* nick, size_t nickcap, double* seconds)
{
	bool known = false;

	known_lock();
	{
		KnownId* e = accid ? known_find(accid) : NULL;
		if (e)
		{
			known = true;
			if (nick && nickcap)
				snprintf(nick, nickcap, "%s", e->nick);
			if (seconds)
				*seconds = e->seconds;
		}
	}
	MutexUnlock(g_knownMut);

	return known;
}

static cJSON* handle_checkid(const cJSON* req)
{
	cJSON* resp = cJSON_CreateObject();

	cJSON* jid = cJSON_GetObjectItemCaseSensitive(req, "id");
	const char* id = cJSON_GetStringValue(jid);

	if (!id || id[0] == '\0')
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "missing id");
		return resp;
	}

	char nick[32] = "";
	double seconds = 0;
	bool known = admin_profile(id, nick, sizeof(nick), &seconds);

	cJSON_AddBoolToObject(resp, "ok", true);
	cJSON_AddBoolToObject(resp, "known", known);
	if (known)
	{
		cJSON_AddStringToObject(resp, "nick", nick);
		cJSON_AddNumberToObject(resp, "seconds", seconds);
	}

	return resp;
}

static cJSON* handle_profile(const cJSON* req)
{
	cJSON* resp = cJSON_CreateObject();

	cJSON* jid = cJSON_GetObjectItemCaseSensitive(req, "id");
	const char* id = cJSON_GetStringValue(jid);

	if (!id || id[0] == '\0')
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "missing id");
		return resp;
	}

	char nick[32] = "";
	double seconds = 0;
	bool known = admin_profile(id, nick, sizeof(nick), &seconds);

	cJSON_AddBoolToObject(resp, "ok", true);
	cJSON_AddBoolToObject(resp, "known", known);
	if (known)
	{
		cJSON_AddStringToObject(resp, "nick", nick);
		cJSON_AddNumberToObject(resp, "seconds", seconds);
	}

	return resp;
}

static cJSON* handle_custom_id(const cJSON* req)
{
	cJSON* resp = cJSON_CreateObject();
	const char* action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "action"));
	const char* raw_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "id"));
	if (!action || !raw_id || !raw_id[0])
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "missing action or id");
		return resp;
	}

	char accid[16];
	snprintf(accid, sizeof(accid), "%s", raw_id);
	for (char* p = accid; *p; p++)
		*p = (char)tolower((unsigned char)*p);

	if (strcmp(action, "get") == 0)
	{
		char value[19];
		if (!custom_id_get(accid, value, sizeof(value)))
		{
			cJSON_AddBoolToObject(resp, "ok", false);
			cJSON_AddStringToObject(resp, "error", "failed to read custom id");
			return resp;
		}
		cJSON_AddBoolToObject(resp, "ok", true);
		cJSON_AddStringToObject(resp, "system_id", accid);
		cJSON_AddStringToObject(resp, "custom_id",
			strcmp(value, accid) == 0 ? "" : value);
		return resp;
	}

	if (strcmp(action, "reset") == 0)
	{
		bool had_custom = false;
		char current[19];
		if (custom_id_get(accid, current, sizeof(current)))
			had_custom = strcmp(current, accid) != 0;
		bool ok = custom_id_reset(accid);
		cJSON_AddBoolToObject(resp, "ok", ok);
		cJSON_AddBoolToObject(resp, "had_custom", had_custom);
		if (!ok)
			cJSON_AddStringToObject(resp, "error", "failed to reset custom id");
		return resp;
	}

	if (strcmp(action, "set") == 0)
	{
		const char* value = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(req, "custom_id"));
		if (!value || !custom_id_set(accid, value))
		{
			cJSON_AddBoolToObject(resp, "ok", false);
			cJSON_AddStringToObject(resp, "error", "invalid or occupied custom id");
			return resp;
		}

		for (int i = 0; i < disaster_count(); i++)
		{
			Server* server = disaster_get(i);
			if (!server)
				continue;
			MutexLock(server->state_lock);
			for (size_t p = 0; p < server->peers.capacity; p++)
			{
				PeerData* peer = (PeerData*)server->peers.ptr[p];
				if (peer && strcmp(peer->accid, accid) == 0)
					custom_id_get(accid, peer->custom_id, sizeof(peer->custom_id));
			}
			MutexUnlock(server->state_lock);
		}

		char normalized[19];
		custom_id_get(accid, normalized, sizeof(normalized));
		cJSON_AddBoolToObject(resp, "ok", true);
		cJSON_AddStringToObject(resp, "system_id", accid);
		cJSON_AddStringToObject(resp, "custom_id", normalized);
		return resp;
	}

	cJSON_AddBoolToObject(resp, "ok", false);
	cJSON_AddStringToObject(resp, "error", "unknown custom id action");
	return resp;
}

static cJSON* handle_known(const cJSON* req)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "players");
	cJSON* joffset = cJSON_GetObjectItemCaseSensitive(req, "offset");
	cJSON* jlimit = cJSON_GetObjectItemCaseSensitive(req, "limit");
	int offset = cJSON_IsNumber(joffset) ? (int)cJSON_GetNumberValue(joffset) : 0;
	int limit = cJSON_IsNumber(jlimit) ? (int)cJSON_GetNumberValue(jlimit) : 100;
	if (offset < 0)
		offset = 0;
	if (limit < 1 || limit > 100)
		limit = 100;

	known_lock();
	{
		cJSON_AddNumberToObject(resp, "total", g_known_n);
		int end = offset + limit;
		if (end > g_known_n)
			end = g_known_n;
		for (int i = offset; i < end; i++)
		{
			cJSON* item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "id", g_known[i].accid);
			cJSON_AddStringToObject(item, "nick", g_known[i].nick);
			cJSON_AddNumberToObject(item, "seconds", g_known[i].seconds);
			cJSON_AddItemToArray(arr, item);
		}
	}
	MutexUnlock(g_knownMut);

	return resp;
}

static cJSON* handle_joins(void)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "joins");

	join_lock();
	{
		int total = g_joins_full ? JOINLOG_CAP : g_joins_head;

		for (int i = 0; i < total; i++)
		{
			int idx = (g_joins_head - 1 - i + JOINLOG_CAP * 2) % JOINLOG_CAP;
			JoinLogEntry* e = &g_joins[idx];

			cJSON* item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "time", e->time);
			cJSON_AddStringToObject(item, "nick", e->nick);
			cJSON_AddStringToObject(item, "accid", e->accid);
			cJSON_AddStringToObject(item, "udid", e->udid);
			cJSON_AddNumberToObject(item, "id", e->id);
			cJSON_AddItemToArray(arr, item);
		}
	}
	MutexUnlock(g_joinMut);

	return resp;
}

static cJSON* handle_news(void)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "news");

	size_t count = news_count();
	char text[2049]; // NEWS_MAX_TEXT из News.c

	for (size_t i = 0; i < count; i++)
	{
		uint32_t id = 0;
		if (!news_get(i, &id, text, sizeof(text)))
			continue;

		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "id", (double)id);
		cJSON_AddStringToObject(item, "text", text);
		cJSON_AddItemToArray(arr, item);
	}

	return resp;
}

static cJSON* handle_newsdel(const cJSON* req)
{
	cJSON* resp = cJSON_CreateObject();

	cJSON* jid = cJSON_GetObjectItemCaseSensitive(req, "id");
	if (!cJSON_IsNumber(jid))
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "missing news id");
		return resp;
	}

	bool ok = news_delete((uint32_t)cJSON_GetNumberValue(jid));
	cJSON_AddBoolToObject(resp, "ok", ok);
	if (!ok)
		cJSON_AddStringToObject(resp, "error", "no such news");

	return resp;
}

// ------------------------------------------------------------------ operators
// операторы сервера хранятся в Operators.json в виде { ip: "Nick (ip)" }

static cJSON* handle_bans(void)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "bans");

	MutexLock(g_banMut);
	{
		cJSON* ban = NULL;
		cJSON_ArrayForEach(ban, g_bans)
		{
			if (!ban->string)
				continue;

			cJSON* item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "key", ban->string);

			cJSON* nick = cJSON_GetObjectItemCaseSensitive(ban, "nickname");
			cJSON_AddStringToObject(item, "nick", cJSON_IsString(nick) ? cJSON_GetStringValue(nick) : "?");

			cJSON* expires = cJSON_GetObjectItemCaseSensitive(ban, "expires");
			cJSON_AddNumberToObject(item, "expires", cJSON_IsNumber(expires) ? cJSON_GetNumberValue(expires) : 0);

			cJSON* reason = cJSON_GetObjectItemCaseSensitive(ban, "reason");
			cJSON_AddStringToObject(item, "reason", cJSON_IsString(reason) ? cJSON_GetStringValue(reason) : "");

			cJSON_AddItemToArray(arr, item);
		}
	}
	MutexUnlock(g_banMut);

	return resp;
}

static cJSON* handle_ops(void)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "ops");

	MutexLock(g_opMut);
	{
		cJSON* op = NULL;
		cJSON_ArrayForEach(op, g_ops)
		{
			if (!op->string)
				continue;

			cJSON* item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "ip", op->string);
			cJSON_AddStringToObject(item, "name", cJSON_GetStringValue(op));
			cJSON_AddItemToArray(arr, item);
		}
	}
	MutexUnlock(g_opMut);

	return resp;
}

static cJSON* handle_opadd(const cJSON* req)
{
	cJSON* resp = cJSON_CreateObject();

	cJSON* jsrv = cJSON_GetObjectItemCaseSensitive(req, "server");
	cJSON* jnick = cJSON_GetObjectItemCaseSensitive(req, "nick");
	const char* nick = cJSON_GetStringValue(jnick);

	if (!cJSON_IsNumber(jsrv) || !nick || nick[0] == '\0')
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "missing server or nick");
		return resp;
	}

	// ники хранятся в нижнем регистре
	char low[32];
	snprintf(low, sizeof(low), "%s", nick);
	for (char* p = low; *p; p++)
		*p = (char)tolower((unsigned char)*p);

	// ищем игрока: в указанном лобби, а если он там не найден - по всем лоббиям хоста
	PeerData* target = NULL;
	Server* target_srv = NULL;

	for (int s = 0; s < disaster_count() && !target; s++)
	{
		Server* server = ((int)cJSON_GetNumberValue(jsrv) == s) ? disaster_get(s) : NULL;
		if (!cJSON_IsNumber(jsrv) || (int)cJSON_GetNumberValue(jsrv) < 0)
			server = disaster_get(s); // сервер не указан - ищем везде
		if (!server)
			continue;

		MutexLock(server->state_lock);
		{
			for (size_t i = 0; i < server->peers.capacity; i++)
			{
				PeerData* peer = (PeerData*)server->peers.ptr[i];
				if (peer && strcmp(peer->nickname.value, low) == 0)
				{
					target = peer;
					target_srv = server;
					break;
				}
			}
		}
		MutexUnlock(server->state_lock);
	}

	if (!target)
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "player isn't online");
		return resp;
	}

	if (!op_add(target->nickname.value, target->ip.value))
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "failed to save operator");
		return resp;
	}

	cJSON_AddBoolToObject(resp, "ok", true);
	cJSON_AddStringToObject(resp, "output", target->nickname.value);
	return resp;
}

static cJSON* handle_opdel(const cJSON* req)
{
	cJSON* resp = cJSON_CreateObject();

	cJSON* jip = cJSON_GetObjectItemCaseSensitive(req, "ip");
	const char* ip = cJSON_GetStringValue(jip);

	if (!ip || ip[0] == '\0')
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "missing ip");
		return resp;
	}

	bool ok = op_revoke(ip);
	cJSON_AddBoolToObject(resp, "ok", ok);
	if (!ok)
		cJSON_AddStringToObject(resp, "error", "no such operator");

	return resp;
}

static cJSON* handle_cmds(void)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "cmds");

	char buf[512];
	console_help_text(buf, sizeof(buf));

	char* p = buf;
	while (*p)
	{
		char* nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';

		cJSON_AddItemToArray(arr, cJSON_CreateString(p));
		if (!nl)
			break;
		p = nl + 1;
	}

	return resp;
}

static cJSON* handle_cmdlog(void)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "log");

	MutexLock(g_eventMut);
	{
		int total = g_events_full ? EVENTLOG_CAP : g_events_head;
		for (int i = 0; i < total; i++)
		{
			int idx = (g_events_head - 1 - i + EVENTLOG_CAP * 2) % EVENTLOG_CAP;
			ChatLogEntry* e = &g_events[idx];
			if (!e->command)
				continue;

			cJSON* item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "time", e->time);
			cJSON_AddNumberToObject(item, "server", e->server);
			cJSON_AddStringToObject(item, "nick", e->nick);
			cJSON_AddStringToObject(item, "accid", e->accid);
			cJSON_AddNumberToObject(item, "player_id", e->player_id);
			cJSON_AddStringToObject(item, "line", e->text);
			cJSON_AddStringToObject(item, "src", "game");
			cJSON_AddItemToArray(arr, item);
		}
	}
	MutexUnlock(g_eventMut);

	ConsoleCmdLog entries[CMDLOG_CAP];
	int n = console_cmdlog_get(entries, CMDLOG_CAP);

	for (int i = 0; i < n; i++)
	{
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "time", entries[i].time);
		cJSON_AddStringToObject(item, "src", entries[i].src);
		cJSON_AddStringToObject(item, "line", entries[i].line);
		cJSON_AddItemToArray(arr, item);
	}

	return resp;
}

static cJSON* handle_chatlog(int server)
{
	cJSON* resp = cJSON_CreateObject();
	cJSON* arr = cJSON_AddArrayToObject(resp, "messages");

	MutexLock(g_eventMut);
	{
		int total = g_events_full ? EVENTLOG_CAP : g_events_head;
		for (int i = 0; i < total; i++)
		{
			int idx = (g_events_head - 1 - i + EVENTLOG_CAP * 2) % EVENTLOG_CAP;
			ChatLogEntry* e = &g_events[idx];
			if (server >= 0 && e->server != server)
				continue;

			cJSON* item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "time", e->time);
			cJSON_AddStringToObject(item, "nick", e->nick);
			cJSON_AddStringToObject(item, "accid", e->accid);
			cJSON_AddNumberToObject(item, "id", e->player_id);
			cJSON_AddStringToObject(item, "text", e->text);
			cJSON_AddItemToArray(arr, item);
		}
	}
	MutexUnlock(g_eventMut);

	return resp;
}

static cJSON* handle_exec(int srv_index, const char* line)
{
	cJSON* resp = cJSON_CreateObject();

	// через внешнее API сервер нельзя остановить - только в живой консоли
	if (line[0] == '\0' ||
		strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0 ||
		strncmp(line, "exit ", 5) == 0 || strncmp(line, "quit ", 5) == 0)
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "shutdown is only allowed from the local console");
		return resp;
	}

	Server* server = disaster_get(srv_index);
	if (!server)
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "no such server");
		return resp;
	}

	char out[ADMIN_MAX_LINE];
	if (!console_exec_ctx(srv_index, (char*)line, "bot", out, sizeof(out)))
	{
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "execution failed");
		return resp;
	}

	cJSON_AddBoolToObject(resp, "ok", true);
	cJSON_AddStringToObject(resp, "output", out);
	return resp;
}

static bool admin_send_all(int sock, const char* data, size_t len)
{
	size_t sent = 0;
	while (sent < len)
	{
#ifdef _WIN32
		int n = send(sock, data + sent, (int)(len - sent), 0);
#else
		ssize_t n = send(sock, data + sent, len - sent, 0);
#endif
		if (n <= 0)
			return false;

		sent += (size_t)n;
	}

	return true;
}

// Reads a '\n'-terminated line byte by byte. Returns false on error or overflow.
static bool admin_recv_line(int sock, char* out, size_t cap)
{
	size_t len = 0;
	while (len + 1 < cap)
	{
		char c;
#ifdef _WIN32
		int n = recv(sock, &c, 1, 0);
#else
		ssize_t n = recv(sock, &c, 1, 0);
#endif
		if (n <= 0)
			return false;

		if (c == '\n')
		{
			out[len] = '\0';
			return true;
		}

		if (c != '\r')
			out[len++] = c;
	}

	return false;
}

static void admin_client_thread(void* arg)
{
	int sock = (int)(intptr_t)arg;
	char line[ADMIN_MAX_LINE];

	cJSON* resp = NULL;

	if (admin_recv_line(sock, line, sizeof(line)))
	{
		cJSON* req = cJSON_ParseWithLength(line, strlen(line));
		if (req)
		{
			const char* cmd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "cmd"));
			cJSON* jsrv = cJSON_GetObjectItemCaseSensitive(req, "server");
			if (!cJSON_IsNumber(jsrv))
				jsrv = cJSON_GetObjectItemCaseSensitive(req, "lobby");
			int srv = cJSON_IsNumber(jsrv) ? (int)cJSON_GetNumberValue(jsrv) : 0;

			if (cmd && strcmp(cmd, "status") == 0)
				resp = handle_status();
			else if (cmd && strcmp(cmd, "players") == 0)
				resp = handle_players(srv);
			else if (cmd && strcmp(cmd, "cmds") == 0)
				resp = handle_cmds();
			else if (cmd && strcmp(cmd, "cmdlog") == 0)
				resp = handle_cmdlog();
			else if (cmd && strcmp(cmd, "chat_lobbies") == 0)
				resp = handle_chat_lobbies();
			else if (cmd && strcmp(cmd, "chatlog") == 0)
				resp = handle_chatlog(srv);
			else if (cmd && strcmp(cmd, "joins") == 0)
				resp = handle_joins();
			else if (cmd && strcmp(cmd, "news") == 0)
				resp = handle_news();
			else if (cmd && strcmp(cmd, "newsdel") == 0)
				resp = handle_newsdel(req);
			else if (cmd && strcmp(cmd, "ops") == 0)
				resp = handle_ops();
			else if (cmd && strcmp(cmd, "bans") == 0)
				resp = handle_bans();
			else if (cmd && strcmp(cmd, "checkid") == 0)
				resp = handle_checkid(req);
			else if (cmd && strcmp(cmd, "profile") == 0)
				resp = handle_profile(req);
			else if (cmd && strcmp(cmd, "known") == 0)
				resp = handle_known(req);
			else if (cmd && strcmp(cmd, "custom_id") == 0)
				resp = handle_custom_id(req);
			else if (cmd && strcmp(cmd, "opadd") == 0)
				resp = handle_opadd(req);
			else if (cmd && strcmp(cmd, "opdel") == 0)
				resp = handle_opdel(req);
			else if (cmd && strcmp(cmd, "exec") == 0)
			{
				const char* l = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "line"));
				resp = handle_exec(srv, l ? l : "");
			}
			else
			{
				resp = cJSON_CreateObject();
				cJSON_AddBoolToObject(resp, "ok", false);
				cJSON_AddStringToObject(resp, "error", "unknown command");
			}

			cJSON_Delete(req);
		}
	}

	if (!resp)
	{
		resp = cJSON_CreateObject();
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "bad request");
	}

	char* out = cJSON_PrintUnformatted(resp);
	if (out)
	{
		admin_send_all(sock, out, strlen(out));
		admin_send_all(sock, "\n", 1);
		free(out);
	}

	cJSON_Delete(resp);

	// Полу-закрытие и слив приёмного буфера - как в News.c, чтобы клиент
	// получил ответ до RST от ядра.
#ifdef _WIN32
	{
		DWORD rcv_to = 2000;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_to, sizeof(rcv_to));
		shutdown(sock, SD_SEND);
	}
#else
	{
		struct timeval tv = { 2, 0 };
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		shutdown(sock, SHUT_WR);
	}
#endif

	{
		char drain;
		while (recv(sock, &drain, 1, 0) > 0) {}
	}

#ifdef _WIN32
	closesocket(sock);
#else
	close(sock);
#endif
}

static void admin_listen_thread(void* arg)
{
	(void)arg;

	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
	{
		Err("Admin API: failed to create socket.");
		return;
	}

#ifdef _WIN32
	BOOL reuse = TRUE;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
	int reuse = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)g_config.admin_port);

	if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		Err("Admin API: failed to bind port %d.", g_config.admin_port);
#ifdef _WIN32
		closesocket(listener);
#else
		close(listener);
#endif
		return;
	}

	if (listen(listener, 8) < 0)
	{
		Err("Admin API: failed to listen on port %d.", g_config.admin_port);
#ifdef _WIN32
		closesocket(listener);
#else
		close(listener);
#endif
		return;
	}

	Info("Admin API listening on port %d.", g_config.admin_port);

	for (;;)
	{
		int client = (int)accept(listener, NULL, NULL);
		if (client < 0)
			continue;

		// one throwaway thread per client: admin requests are short-lived
		Thread th;
		ThreadSpawn(th, admin_client_thread, (void*)(intptr_t)client);
	}
}

SERVER_API bool admin_init(void)
{
	MutexCreate(g_eventMut);

	if (g_config.admin_port <= 0 || g_config.admin_port > 65535)
		return true; // feature disabled

	MutexCreate(g_joinMut);
	MutexCreate(g_knownMut);

	// реестр известных ID: { accid: {nick, seconds, first_seen} }
	{
		cJSON* root = NULL;
		FILE* f = fopen(KNOWN_FILE, "r");
		if (f)
		{
			fseek(f, 0, SEEK_END);
			size_t len = ftell(f);
			fseek(f, 0, SEEK_SET);
			char* data = (char*)malloc(len + 1);
			if (data)
			{
				fread(data, 1, len, f);
				data[len] = '\0';
				root = cJSON_ParseWithLength(data, len);
				free(data);
			}
			fclose(f);
		}
		if (root && cJSON_IsObject(root))
		{
			cJSON* item = NULL;
			cJSON_ArrayForEach(item, root)
			{
				if (!item->string || g_known_n >= KNOWN_CAP)
					continue;

				KnownId* e = &g_known[g_known_n++];
				memset(e, 0, sizeof(*e));
				snprintf(e->accid, sizeof(e->accid), "%s", item->string);

				cJSON* nick = cJSON_GetObjectItemCaseSensitive(item, "nick");
				cJSON* seconds = cJSON_GetObjectItemCaseSensitive(item, "seconds");
				cJSON* seen = cJSON_GetObjectItemCaseSensitive(item, "first_seen");
				snprintf(e->nick, sizeof(e->nick), "%s", cJSON_IsString(nick) ? cJSON_GetStringValue(nick) : "");
				e->seconds = cJSON_IsNumber(seconds) ? cJSON_GetNumberValue(seconds) : 0;
				e->first_seen = cJSON_IsNumber(seen) ? (uint64_t)cJSON_GetNumberValue(seen) : 0;
			}
			Info("Admin API: %d known account ids loaded.", g_known_n);
		}
		if (root)
			cJSON_Delete(root);
	}

	Thread th;
	ThreadSpawn(th, admin_listen_thread, NULL);
	return true;
}

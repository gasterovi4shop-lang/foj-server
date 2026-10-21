#include "Server.h"
#include <Admin.h>
#include <States.h>
#include <Maps.h>
#include <Config.h>
#include <CMath.h>
#include <Colors.h>
#include <ctype.h>
#include <time.h>

// seconds a player has to ban a map before a random one is banned
#define DRAFT_PICK_TIME (15)

// the peer whose turn it currently is (NULL if they left)
static PeerData* mapdraft_turn_peer(Server* server)
{
	Lobby* lobby = &server->lobby;
	if (lobby->draft_turn >= lobby->draft_order_count)
		return NULL;

	return server_find_peer(server, lobby->draft_order[lobby->draft_turn]);
}

// announce the current turn player and restart the pick timer;
// skips over players who have left, returns false if nobody is left
static bool mapdraft_announce_turn(Server* server)
{
	Lobby* lobby = &server->lobby;
	if (lobby->draft_order_count == 0)
		return false;

	for (uint8_t tried = 0; tried < lobby->draft_order_count; tried++)
	{
		uint8_t idx = (lobby->draft_turn + tried) % lobby->draft_order_count;
		PeerData* peer = server_find_peer(server, lobby->draft_order[idx]);
		if (!peer || !peer->in_game)
			continue;

		lobby->draft_turn = idx;

		char buffer[320];
		snprintf(buffer, 320, "%s ~is " CLRCODE_GRN "choosing" CLRCODE_RST " map for " CLRCODE_RED "ban", peer->nickname.value);
		server_broadcast_msg(server, buffer);

		lobby->draft_timer = TICKSPERSEC;
		lobby->draft_timer_sec = DRAFT_PICK_TIME;

		Packet pack;
		PacketCreate(&pack, SERVER_DRAFT_TIME);
		PacketWrite(&pack, packet_write8, lobby->draft_timer_sec);
		server_broadcast(server, &pack, true);
		return true;
	}

	return false;
}

// the draft is over: the last remaining map wins. The event simply ends in
// place - the players never left the lobby room, so nothing is recreated;
// the usual ready -> 3-2-1 countdown follows and skips the 3 map vote
static bool mapdraft_finish(Server* server, uint8_t map)
{
	server->last_map = map;
	server->lobby.mapdraft_map = map;

	// plain lobby state again, match countdown armed from scratch
	server->state = ST_LOBBY;
	server->lobby.countdown = TICKSPERSEC;
	server->lobby.countdown_sec = LOBBY_NO_COUNTDOWN;
	server->lobby.vote.ongoing = 0;

	// the AFK window restarts: the draft itself took long enough
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* peer = (PeerData*)server->peers.ptr[i];
		if (peer)
			peer->timeout = 0;
	}

	// hide the pick timer on the clients (0 = hide)
	Packet pack;
	PacketCreate(&pack, SERVER_DRAFT_TIME);
	PacketWrite(&pack, packet_write8, 0);
	server_broadcast(server, &pack, true);

	char buffer[128];
	snprintf(buffer, 128, "next " CLRCODE_GRN "map" CLRCODE_RST " is %s", g_mapList[map].name);
	server_broadcast_msg(server, buffer);

	Info(LOG_YLW "Map draft picked [" LOG_BLU "%s" LOG_RST "]", g_mapList[map].name);

	// everyone stayed ready from before the draft: start the 3-2-1 countdown
	// right away; if somebody unreadied, it starts once they ready up again
	RAssert(lobby_check_countdown(server));

	return true;
}

// remove a map from the pool and pass the turn (or finish the draft)
static bool mapdraft_ban_map(Server* server, uint8_t map)
{
	Lobby* lobby = &server->lobby;

	for (uint8_t i = 0; i < lobby->draft_pool_count; i++)
	{
		if (lobby->draft_pool[i] != map)
			continue;

		lobby->draft_pool[i] = lobby->draft_pool[lobby->draft_pool_count - 1];
		lobby->draft_pool_count--;
		break;
	}

	Info(LOG_YLW "Map draft: [" LOG_BLU "%s" LOG_RST "] banned, %d left", g_mapList[map].name, lobby->draft_pool_count);

	if (lobby->draft_pool_count <= 1)
	{
		if (lobby->draft_pool_count == 1)
			return mapdraft_finish(server, lobby->draft_pool[0]);

		return lobby_init(server);
	}

	lobby->draft_turn = (lobby->draft_turn + 1) % lobby->draft_order_count;
	if (!mapdraft_announce_turn(server))
		return lobby_init(server);

	return true;
}

bool mapdraft_init(Server* server)
{
	Debug("Attepting to enter ST_MAPDRAFT...");
	RAssert(server);

	// the armed flag is consumed: one draft per vote
	server->lobby.mapdraft = false;

	// any lobby vote still running is irrelevant now
	server->lobby.vote.ongoing = 0;
	server->lobby.prac_countdown = 0;

	// build the pool: Fart Zone is not part of MAP_COUNT and maps disabled
	// in the server panel are counted as pre-banned
	MutexLock(g_config.map_list_lock);
	{
		server->lobby.draft_pool_count = 0;
		for (uint8_t i = 0; i < MAP_COUNT; i++)
		{
			if (g_config.map_list[i])
				server->lobby.draft_pool[server->lobby.draft_pool_count++] = i;
		}
	}
	MutexUnlock(g_config.map_list_lock);

	// turn order is the lobby join order (peer ids grow with each connection)
	server->lobby.draft_order_count = 0;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* peer = (PeerData*)server->peers.ptr[i];
		if (!peer || !peer->in_game)
			continue;

		// insertion sort by id
		uint8_t pos = server->lobby.draft_order_count++;
		while (pos > 0 && server->lobby.draft_order[pos - 1] > peer->id)
		{
			server->lobby.draft_order[pos] = server->lobby.draft_order[pos - 1];
			pos--;
		}
		server->lobby.draft_order[pos] = peer->id;
	}

	if (server->lobby.draft_order_count == 0)
		return lobby_init(server);

	// nobody can ban anything - fall back to the regular map vote
	if (server->lobby.draft_pool_count == 0)
		return mapvote_init(server);

	server->state = ST_MAPDRAFT;
	server->lobby.draft_timer = TICKSPERSEC;
	server->lobby.draft_timer_sec = DRAFT_PICK_TIME;
	server->lobby.draft_turn = 0;

	server_broadcast_msg(server, "-----------------------");
	server_broadcast_msg(server, CLRCODE_GRN "map draft" CLRCODE_RST " ban maps with .b 1-21 or .b map name");
	server_broadcast_msg(server, "-----------------------");

	Info(LOG_YLW "Server is now in " LOG_PUR "Map Draft (pool %d, players %d)", server->lobby.draft_pool_count, server->lobby.draft_order_count);

	// a single map left means there is nothing to ban
	if (server->lobby.draft_pool_count == 1)
		return mapdraft_finish(server, server->lobby.draft_pool[0]);

	RAssert(mapdraft_announce_turn(server));
	return true;
}

bool mapdraft_state_handle(PeerData* v, Packet* packet)
{
	// Read header
	PacketRead(passtrough, packet, packet_read8, uint8_t);
	PacketRead(type, packet, packet_read8, uint8_t);

	Packet pack;
	switch (type)
	{
	case CLIENT_CHAT_MESSAGE:
	{
		PacketRead(pid, packet, packet_read16, uint16_t);
		PacketRead(msg, packet, packet_readstr, String);
		//слишком длинное сообщение обрезается до 40 символов вместо кика клиента
		packet_truncate_chatmsg(packet, 40);
		admin_log_chat(v->server->id, v->nickname.value, v->accid, v->id, msg.value);

		v->timeout = 0;
		Info("%s " LOG_RST "(id %d): %s", v->nickname.value, v->id, msg.value);
		if (!server_cmd_handle(v->server, server_cmd_parse(&msg), v, &msg))
			server_broadcast_ex(v->server, packet, true, v->id);

		break;
	}

	case CLIENT_LOBBY_READY_STATE:
	{
		// the match cannot start during the draft - keep the ready icons in
		// sync but don't run the start countdown
		PacketRead(state, packet, packet_read8, uint8_t);
		v->ready = state;

		PacketCreate(&pack, SERVER_LOBBY_READY_STATE);
		PacketWrite(&pack, packet_write16, v->id);
		PacketWrite(&pack, packet_write8, state);
		server_broadcast(v->server, &pack, true);
		break;
	}

	default:
		RAssert(server_msg_handle(v->server, type, v, packet));
		break;
	}

	return true;
}

bool mapdraft_state_tick(Server* server)
{
	if (server->lobby.draft_timer <= 0)
	{
		server->lobby.draft_timer += TICKSPERSEC;

		if (--server->lobby.draft_timer_sec == 0)
		{
			// out of time - a random map from the remaining pool is banned
			Lobby* lobby = &server->lobby;
			uint8_t map = lobby->draft_pool[rand() % lobby->draft_pool_count];
			PeerData* turn = mapdraft_turn_peer(server);

			char buffer[320];
			if (turn)
			{
				snprintf(buffer, 320, "%s " CLRCODE_RED "timed out" CLRCODE_RST, turn->nickname.value);
				server_broadcast_msg(server, buffer);
			}

			snprintf(buffer, 320, "%s " CLRCODE_RED "was banned" CLRCODE_RST, g_mapList[map].name);
			server_broadcast_msg(server, buffer);

			return mapdraft_ban_map(server, map);
		}

		Packet pack;
		PacketCreate(&pack, SERVER_DRAFT_TIME);
		PacketWrite(&pack, packet_write8, server->lobby.draft_timer_sec);
		server_broadcast(server, &pack, true);
	}

	server->lobby.draft_timer -= server->delta;
	return true;
}

bool mapdraft_state_join(PeerData* v)
{
	return true;
}

bool mapdraft_state_left(PeerData* v)
{
	Server* server = v->server;
	Lobby* lobby = &server->lobby;

	// drop the leaver from the turn order, remembering where they sat
	uint8_t count = lobby->draft_order_count;
	uint8_t write = 0;
	int removed_at = -1;
	for (uint8_t i = 0; i < count; i++)
	{
		if (lobby->draft_order[i] == v->id)
		{
			removed_at = i;
			continue;
		}

		lobby->draft_order[write++] = lobby->draft_order[i];
	}
	lobby->draft_order_count = write;

	if (server_ingame(server) <= 1 || lobby->draft_order_count == 0)
		return lobby_init(server);

	if (removed_at >= 0)
	{
		if (removed_at < lobby->draft_turn)
			lobby->draft_turn--;
		else if (removed_at == lobby->draft_turn)
		{
			// it was the leaver's turn - pass it on with a fresh timer
			lobby->draft_turn %= lobby->draft_order_count;
			return mapdraft_announce_turn(server) || lobby_init(server);
		}
	}

	return true;
}

// ".b <1-21>" or ".b <map name>": ban a map while the draft is running;
// outside ST_MAPDRAFT this returns false so the message stays regular chat
bool mapdraft_cmd_ban(PeerData* v, String* msg)
{
	Server* server = v->server;
	if (server->state != ST_MAPDRAFT)
		return false;

	Lobby* lobby = &server->lobby;

	if (lobby->draft_order_count == 0)
		return true;

	PeerData* turn = mapdraft_turn_peer(server);
	if (!turn || turn->id != v->id)
	{
		RAssert(server_send_msg(server, v->peer, CLRCODE_RED "not allowed." CLRCODE_RST " it's not your turn"));
		return true;
	}

	// argument: whatever follows the command token
	long num = -1;
	char name[64] = { 0 };
	bool isnum = sscanf(msg->value, "%*s %ld", &num) == 1;
	if (!isnum && sscanf(msg->value, "%*s %63[^\n]", name) != 1)
	{
		RAssert(server_send_msg(server, v->peer, CLRCODE_RED "usage:" CLRCODE_RST " .b 1-21 or .b map name"));
		return true;
	}

	// trim trailing whitespace from the name
	for (char* p = name + strlen(name); p > name && isspace((unsigned char)p[-1]); p--)
		p[-1] = '\0';

	if (!isnum && name[0] == '\0')
	{
		RAssert(server_send_msg(server, v->peer, CLRCODE_RED "usage:" CLRCODE_RST " .b 1-21 or .b map name"));
		return true;
	}

	uint8_t map;
	if (isnum)
	{
		if (num < 1 || num > MAP_COUNT)
		{
			char err[100];
			snprintf(err, 100, CLRCODE_RED "map number should be 1 to %d", MAP_COUNT);
			RAssert(server_send_msg(server, v->peer, err));
			return true;
		}

		map = (uint8_t)(num - 1);
	}
	else
	{
		// map names carry a color code prefix, so match lowercase against
		// the lowercased full name: exact text is the suffix of it
		for (char* p = name; *p != '\0'; p++)
			*p = (char)tolower((unsigned char)*p);

		size_t len = strlen(name);
		bool found = false;

		for (uint8_t i = 0; i < MAP_COUNT && !found; i++)
		{
			char hay[64];
			snprintf(hay, sizeof(hay), "%s", g_mapList[i].name);
			for (char* p = hay; *p != '\0'; p++)
				*p = (char)tolower((unsigned char)*p);

			size_t haylen = strlen(hay);
			if (haylen >= len && strcmp(hay + haylen - len, name) == 0)
			{
				map = i;
				found = true;
			}
		}

		// no exact name - accept a single substring match (".b volcano")
		if (!found)
		{
			for (uint8_t i = 0; i < MAP_COUNT; i++)
			{
				char hay[64];
				snprintf(hay, sizeof(hay), "%s", g_mapList[i].name);
				for (char* p = hay; *p != '\0'; p++)
					*p = (char)tolower((unsigned char)*p);

				if (!strstr(hay, name))
					continue;

				if (found)
				{
					RAssert(server_send_msg(server, v->peer, CLRCODE_RED "ambiguous map name" CLRCODE_RST " - be more specific"));
					return true;
				}

				map = i;
				found = true;
			}
		}

		if (!found)
		{
			RAssert(server_send_msg(server, v->peer, CLRCODE_RED "map not found" CLRCODE_RST " - .b 1-21 or .b map name"));
			return true;
		}
	}

	for (uint8_t i = 0; i < lobby->draft_pool_count; i++)
	{
		if (lobby->draft_pool[i] == map)
		{
			char buffer[320];
			snprintf(buffer, 320, "%s " CLRCODE_RED "banned" CLRCODE_RST " %s", v->nickname.value, g_mapList[map].name);
			server_broadcast_msg(server, buffer);

			return mapdraft_ban_map(server, map);
		}
	}

	RAssert(server_send_msg(server, v->peer, CLRCODE_RED "already banned" CLRCODE_RST));
	return true;
}

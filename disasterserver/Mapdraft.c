#include "Server.h"
#include <Admin.h>
#include <States.h>
#include <Maps.h>
#include <Config.h>
#include <CMath.h>
#include <Colors.h>
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

// a map counts as banned once it is out of the remaining pool (this also
// covers the maps disabled in the server panel: they never enter the pool)
static bool mapdraft_is_banned(Server* server, uint8_t map)
{
	for (uint8_t i = 0; i < server->lobby.draft_pool_count; i++)
	{
		if (server->lobby.draft_pool[i] == map)
			return false;
	}

	return true;
}

static uint8_t mapdraft_first_free(Server* server)
{
	for (uint8_t i = 0; i < MAP_COUNT; i++)
	{
		if (!mapdraft_is_banned(server, i))
			return i;
	}

	return 0;
}

// full grid snapshot: banned bitfield + whose turn it is + their cursor
static bool mapdraft_broadcast_state(Server* server)
{
	Lobby* lobby = &server->lobby;
	PeerData* turn = mapdraft_turn_peer(server);

	Packet pack;
	PacketCreate(&pack, SERVER_DRAFT_STATE);
	for (uint8_t b = 0; b < (MAP_COUNT + 7) / 8; b++)
	{
		uint8_t bits = 0;
		for (uint8_t i = b * 8; i < b * 8 + 8 && i < MAP_COUNT; i++)
		{
			if (mapdraft_is_banned(server, i))
				bits |= 1 << (i - b * 8);
		}
		PacketWrite(&pack, packet_write8, bits);
	}
	PacketWrite(&pack, packet_write16, turn ? turn->id : 0);
	PacketWrite(&pack, packet_write8, lobby->draft_selected);
	server_broadcast(server, &pack, true);
	return true;
}

// hand the turn to the draft_turn player (or the next one still here):
// fresh 15s timer, cursor parked on the first free map
static bool mapdraft_announce_turn(Server* server)
{
	Lobby* lobby = &server->lobby;
	if (lobby->draft_order_count == 0)
		return false;

	for (uint8_t tried = 0; tried < lobby->draft_order_count; tried++)
	{
		uint8_t idx = (lobby->draft_turn + tried) % lobby->draft_order_count;
		PeerData* peer = server_find_peer(server, lobby->draft_order[idx]);
		if (!peer || !peer->in_game || peer->afk)
			continue;

		lobby->draft_turn = idx;
		lobby->draft_selected = mapdraft_first_free(server);
		lobby->draft_timer = TICKSPERSEC;
		lobby->draft_timer_sec = DRAFT_PICK_TIME;

		RAssert(mapdraft_broadcast_state(server));

		// turn status line under the grid: whose pick it is now
		char buffer[160];
		snprintf(buffer, 160, "%s ~is " CLRCODE_GRN "choosing" CLRCODE_RST " a map to " CLRCODE_RED "ban", peer->nickname.value);

		Packet ev;
		PacketCreate(&ev, SERVER_DRAFT_EVENT);
		PacketWrite(&ev, packet_write8, 1);
		PacketWrite(&ev, packet_writestr, string_new(buffer));
		server_broadcast(server, &ev, true);

		Packet pack;
		PacketCreate(&pack, SERVER_DRAFT_TIME);
		PacketWrite(&pack, packet_write8, lobby->draft_timer_sec);
		server_broadcast(server, &pack, true);
		return true;
	}

	return false;
}

// the draft is over: the last remaining map wins and the match continues
// right in character select, exactly like the regular map vote does
static bool mapdraft_finish(Server* server, uint8_t map)
{
	server->last_map = map;

	char buffer[128];
	snprintf(buffer, 128, "next " CLRCODE_GRN "map" CLRCODE_RST " is %s", g_mapList[map].name);
	server_broadcast_msg(server, buffer);

	Info(LOG_YLW "Map draft picked [" LOG_BLU "%s" LOG_RST "]", g_mapList[map].name);

	return charselect_init(map, server) || lobby_init(server);
}

// remove a map from the pool and pass the turn (or finish the draft);
// timed_out: the pick expired and the map was banned randomly
static bool mapdraft_ban_map(Server* server, uint8_t map, bool timed_out)
{
	Lobby* lobby = &server->lobby;
	PeerData* turn = mapdraft_turn_peer(server);

	// event line under the grid: who banned what / who skipped the turn
	char buffer[160];
	if (timed_out)
		snprintf(buffer, 160, "%s " CLRCODE_RED "skipped the turn" CLRCODE_RST " - %s " CLRCODE_RED "was banned", turn ? turn->nickname.value : "?", g_mapList[map].name);
	else
		snprintf(buffer, 160, "%s " CLRCODE_RED "banned" CLRCODE_RST " %s", turn ? turn->nickname.value : "?", g_mapList[map].name);

	Packet ev;
	PacketCreate(&ev, SERVER_DRAFT_EVENT);
	PacketWrite(&ev, packet_write8, 0);
	PacketWrite(&ev, packet_writestr, string_new(buffer));
	server_broadcast(server, &ev, true);

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
		if (!peer || !peer->in_game || peer->afk)
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

	Info(LOG_YLW "Server is now in " LOG_PUR "Map Draft (pool %d, players %d)", server->lobby.draft_pool_count, server->lobby.draft_order_count);

	// a single map left means there is nothing to ban: skip the grid entirely
	if (server->lobby.draft_pool_count == 1)
		return mapdraft_finish(server, server->lobby.draft_pool[0]);

	server->state = ST_MAPDRAFT;
	server->lobby.draft_timer = TICKSPERSEC;
	server->lobby.draft_timer_sec = DRAFT_PICK_TIME;
	server->lobby.draft_turn = 0;

	// open the vote grid on the clients, then sync the initial state
	Packet pack;
	PacketCreate(&pack, SERVER_DRAFT_START);
	server_broadcast(server, &pack, true);

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
	case CLIENT_DRAFT_SELECT:
	{
		if (!v->in_game)
			break;

		PacketRead(slot, packet, packet_read8, uint8_t);

		PeerData* turn = mapdraft_turn_peer(v->server);
		if (!turn || turn->id != v->id || slot >= MAP_COUNT)
			break;

		v->server->lobby.draft_selected = slot;
		RAssert(mapdraft_broadcast_state(v->server));
		break;
	}

	case CLIENT_DRAFT_BAN:
	{
		if (!v->in_game)
			break;

		PacketRead(slot, packet, packet_read8, uint8_t);

		Server* server = v->server;
		PeerData* turn = mapdraft_turn_peer(server);
		if (!turn || turn->id != v->id || slot >= MAP_COUNT)
			break;

		// already banned (or disabled in the panel) - ignore
		if (mapdraft_is_banned(server, slot))
			break;

		return mapdraft_ban_map(server, slot, false);
	}

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

			Info(LOG_YLW "Map draft: pick timed out, [" LOG_BLU "%s" LOG_RST "] banned randomly", g_mapList[map].name);

			return mapdraft_ban_map(server, map, true);
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

	// the turn order shifted - resync the grid
	RAssert(mapdraft_broadcast_state(server));
	return true;
}

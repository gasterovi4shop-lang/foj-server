#include <Player.h>
#include <Packet.h>
#include <Server.h>
#include <Admin.h>
#include <Config.h>
#include <Colors.h>
#include <States.h>

#define PLRSTATE_ESCAPED 4
#define PLRSTATE_ALIVE 3
#define PLRSTATE_DEAD 2
#define PLRSTATE_DEMONIZED 1
#define PLRSTATE_EXE 0

const char* SHAMES_1[] = 
{
	CLRCODE_RED "(раньше здесь был мак)",
	CLRCODE_RED "(боюсь, что нет)",
	CLRCODE_RED "(я даже не вспотел)",
	CLRCODE_RED "(хз, не играл)",
	CLRCODE_RED "(гойда гойда гойда)"
};
#define SHAMES1_CNT 5

const char* SHAMES_2[] = 
{
	CLRCODE_RED "(я сосал шпиону из тф2)",
	CLRCODE_RED "(не, ну еще посидим, еще посидим)",
	CLRCODE_RED "(сигма про 1488)"
};
#define SHAMES2_CNT 3

// MVP: exe takes it by taking the round; once at least one survivor escapes
// the exe lost and the best survivor does - stuns/heals decide, the rest breaks ties
#define MVP_STUN_WEIGHT 2
#define MVP_HEAL_WEIGHT 1

static int results_mvp_score(const PeerData* data)
{
	return (int)data->plr.stats.stuns * MVP_STUN_WEIGHT
		+ (int)data->plr.stats.hp_restored * MVP_HEAL_WEIGHT;
}

// true if a deserves MVP more than b: stuns+heals score,
// then stuns, heals, danger time, rings, damage dealt, survive time, less damage taken
static bool results_mvp_better(const PeerData* a, const PeerData* b)
{
	int sa = results_mvp_score(a);
	int sb = results_mvp_score(b);

	if (sa != sb)
		return sa > sb;

	if (a->plr.stats.stuns != b->plr.stats.stuns)
		return a->plr.stats.stuns > b->plr.stats.stuns;

	if (a->plr.stats.hp_restored != b->plr.stats.hp_restored)
		return a->plr.stats.hp_restored > b->plr.stats.hp_restored;

	if (a->plr.stats.danger_time != b->plr.stats.danger_time)
		return a->plr.stats.danger_time > b->plr.stats.danger_time;

	if (a->plr.stats.rings != b->plr.stats.rings)
		return a->plr.stats.rings > b->plr.stats.rings;

	if (a->plr.stats.damage != b->plr.stats.damage)
		return a->plr.stats.damage > b->plr.stats.damage;

	if (a->plr.stats.survive_time != b->plr.stats.survive_time)
		return a->plr.stats.survive_time > b->plr.stats.survive_time;

	return a->plr.stats.damage_taken < b->plr.stats.damage_taken;
}

static void results_calculate_mvp(Server* server)
{
	server->results.mvp_id = -1;

	// the exe loses the round once at least one survivor escapes
	bool exe_lost = false;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* data = (PeerData*)server->peers.ptr[i];
		if (!data || !data->in_game)
			continue;

		if (data->plr.flags & PLAYER_ESCAPED)
			exe_lost = true;
	}

	for (size_t i = 0; i < server->game.left.capacity; i++)
	{
		PeerData* data = (PeerData*)server->game.left.ptr[i];
		if (!data || !data->in_game)
			continue;

		if (data->plr.flags & PLAYER_ESCAPED)
			exe_lost = true;
	}

	if (!exe_lost)
	{
		// nobody got away (exe win or time over) - the exe is the MVP
		for (size_t i = 0; i < server->peers.capacity; i++)
		{
			PeerData* data = (PeerData*)server->peers.ptr[i];
			if (!data || !data->in_game || (data->plr.flags & PLAYER_LEFT))
				continue;

			if (data->id == (uint16_t)server->game.exe)
			{
				server->results.mvp_id = (int16_t)data->id;
				return;
			}
		}
	}

	// exe lost (or is gone): MVP goes to the best player;
	// when the exe lost, his team (exe and demonized) is out
	PeerData* best = NULL;

	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* data = (PeerData*)server->peers.ptr[i];
		if (!data || !data->in_game || (data->plr.flags & PLAYER_LEFT))
			continue;

		if (exe_lost && (data->id == (uint16_t)server->game.exe || (data->plr.flags & PLAYER_DEMONIZED)))
			continue;

		if (!best || results_mvp_better(data, best))
			best = data;
	}

	for (size_t i = 0; i < server->game.left.capacity; i++)
	{
		PeerData* data = (PeerData*)server->game.left.ptr[i];
		if (!data || !data->in_game || (data->plr.flags & PLAYER_LEFT))
			continue;

		if (exe_lost && (data->id == (uint16_t)server->game.exe || (data->plr.flags & PLAYER_DEMONIZED)))
			continue;

		if (!best || results_mvp_better(data, best))
			best = data;
	}

	if (best)
		server->results.mvp_id = (int16_t)best->id;
}

int compare(const PeerData** plr1, const PeerData** plr2)
{
	const PeerData* a = *plr1;
	const PeerData* b = *plr2;

	if (a->plr.flags & PLAYER_KILLER || b->plr.flags & PLAYER_KILLER)
		return ((b->plr.flags & PLAYER_KILLER) > 0) - ((a->plr.flags & PLAYER_KILLER) > 0);

	if (a->plr.flags & PLAYER_LEFT || b->plr.flags & PLAYER_LEFT)
		return ((a->plr.flags & PLAYER_LEFT) > 0) - ((b->plr.flags & PLAYER_LEFT) > 0);

	if (a->plr.flags & PLAYER_DEMONIZED || b->plr.flags & PLAYER_DEMONIZED)
		return ((a->plr.flags & PLAYER_DEMONIZED) > 0) - ((b->plr.flags & PLAYER_DEMONIZED) > 0);

	if (a->plr.flags & PLAYER_DEAD || b->plr.flags & PLAYER_DEAD)
		return ((a->plr.flags & PLAYER_DEAD) > 0) - ((b->plr.flags & PLAYER_DEAD) > 0);

	return 0;
}

int compare2(const PeerData** plr1, const PeerData** plr2)
{
	const PeerData* a = *plr1;
	const PeerData* b = *plr2;

	if (a->plr.flags & PLAYER_KILLER || b->plr.flags & PLAYER_KILLER)
		return ((b->plr.flags & PLAYER_KILLER) > 0) - ((a->plr.flags & PLAYER_KILLER) > 0);

	if (a->plr.flags & PLAYER_LEFT || b->plr.flags & PLAYER_LEFT)
		return 0;

	if (a->plr.flags & PLAYER_DEMONIZED || b->plr.flags & PLAYER_DEMONIZED)
		return 0;

	if ((a->plr.flags & PLAYER_DEAD) > 0 == (b->plr.flags & PLAYER_DEAD) > 0)
		return (int)(b->plr.stats.danger_time - a->plr.stats.danger_time);

	return 0;
}

bool results_send(Server* server, PeerData* v, PeerData* data, bool has_quit)
{
	uint8_t type = PLRSTATE_ALIVE;

	if (data->plr.flags & PLAYER_ESCAPED)
		type = PLRSTATE_ESCAPED;
	if (data->plr.flags & PLAYER_DEMONIZED)
		type = PLRSTATE_DEMONIZED;
	else if (data->plr.flags & PLAYER_DEAD)
		type = PLRSTATE_DEAD;
	else if (server->game.exe == data->id)
		type = PLRSTATE_EXE;

	Packet pack;
	PacketCreate(&pack, SERVER_RESULTS_DATA);

	String nickname;
	const char* postfix = "";

	if (g_config.pride)
	{
		if (data->plr.stats.brain_damage && (data->plr.flags & PLAYER_ESCAPED))
			postfix = SHAMES_1[rand() % SHAMES1_CNT];

		if (data->plr.stats.camp_time >= 30 * TICKSPERSEC)
			postfix = SHAMES_2[rand() % SHAMES2_CNT];
	}

	nickname.len = snprintf(nickname.value, 129, "%s %s", data->nickname.value, postfix) + 1;

	// short account id (or custom id) - the client draws it after the nick
	String dispid;
	char dispbuf[19];
	custom_id_get(data->accid, dispbuf, sizeof(dispbuf));
	dispid.len = snprintf(dispid.value, sizeof(dispid.value), "%s", dispbuf) + 1;

	PacketWrite(&pack, packet_writestr, nickname);
	PacketWrite(&pack, packet_writestr, dispid);
	PacketWrite(&pack, packet_write8, 	data->exe_char != EX_NONE ? data->exe_char : data->surv_char);
	PacketWrite(&pack, packet_write8,	server->game.ending);
	PacketWrite(&pack, packet_write16,	server->game.time_sec)
	PacketWrite(&pack, packet_write8,	has_quit);
	PacketWrite(&pack, packet_write8,	type);

	PacketWrite(&pack, packet_write16,		data->plr.stats.rings);
	PacketWrite(&pack, packet_write16,		data->plr.stats.kills);
	PacketWrite(&pack, packet_write16,		data->plr.stats.damage);
	PacketWrite(&pack, packet_write16,		data->plr.stats.damage_taken);
	PacketWrite(&pack, packet_write16,		data->plr.stats.stun_time);
	PacketWrite(&pack, packet_write16,		data->plr.stats.stuns);
	PacketWrite(&pack, packet_write16,		data->plr.stats.hp_restored);
	PacketWrite(&pack, packet_writedouble,	data->plr.stats.survive_time);
	PacketWrite(&pack, packet_writedouble,	data->plr.stats.danger_time);
	PacketWrite(&pack, packet_write8,		data->id == (uint16_t)server->results.mvp_id);

	return packet_send(v->peer, &pack, true);
}

bool results_init(Server* server)
{
	Debug("Attepting to enter ST_RESULTS...");
	server->state = ST_RESULTS;
	server->results.countdown = 15 * TICKSPERSEC;
	results_calculate_mvp(server);
	Info("Match MVP id is %d", server->results.mvp_id);

	Packet pack;
	PacketCreate(&pack, SERVER_RESULTS);
	PacketWrite(&pack, packet_write8, server->game.map);
	server_broadcast(server, &pack, true);

	Info("Server is now in " LOG_PUR "Results");
	return true;
}

bool results_uninit(Server* server)
{
	for (size_t i = 0; i < server->game.entities.capacity; i++)
	{
		Entity* entity = (Entity*)server->game.entities.ptr[i];
		if (!entity)
			continue;

		free(entity);
	}
	dylist_free(&server->game.entities);

	// Clean up after game
	for (size_t i = 0; i < server->game.left.capacity; i++)
	{
		PeerData* player = (PeerData*)server->game.left.ptr[i];
		if (!player)
			continue;

		free(player);
	}
	dylist_free(&server->game.left);

	return lobby_init(server);
}

bool results_state_tick(Server* server)
{
	server->results.countdown -= server->delta;
	
	if (server->results.countdown <= 0)
		return results_uninit(server);

	return true;
}

bool results_state_handle(PeerData* v, Packet* packet)
{
	PacketRead(_passtrough, packet, packet_read8, uint8_t);
	PacketRead(type, packet, packet_read8, uint8_t);

	switch (type) 
	{
		case CLIENT_RESULTS_REQUEST:
		{
			PeerData* sort[7];
			int len = 0;
			memset(sort, 0, sizeof(PeerData*) * 7);

			for (size_t i = 0; i < v->server->peers.capacity; i++)
			{
				PeerData* data = (PeerData*)v->server->peers.ptr[i];
				if (!data)
					continue;

				if(!data->in_game)
					continue;

				sort[len++] = data;
			}

			for (size_t i = 0; i < v->server->game.left.capacity; i++)
			{
				PeerData* data = (PeerData*)v->server->game.left.ptr[i];
				if (!data)
					continue;

				if(!data->in_game)
					continue;

				sort[len++] = data;
			}

			qsort(sort, len, sizeof(PeerData*), (int (*)(const void *, const void *))compare);
			qsort(sort, len, sizeof(PeerData*), (int (*)(const void *, const void *))compare2);

			for (size_t i = 0; i < len; i++)
			{
				PeerData* data = (PeerData*)sort[i];
				if (!data)
					continue;

				RAssert(results_send(v->server, v, data, data->plr.flags & PLAYER_LEFT));
			}
			break;
		}
		
		case CLIENT_CHAT_MESSAGE:
		{
			if (v->in_game)
				break;

			PacketRead(pid, packet, packet_read16, uint16_t);
			PacketRead(msg, packet, packet_readstr, String);
			//слишком длинное сообщение обрезается до 40 символов вместо кика клиента
			packet_truncate_chatmsg(packet, 40);
			admin_log_chat(v->server->id, v->nickname.value, v->accid, v->id, msg.value);

			v->timeout = 0;

			Info("[%s] (id %d): %s", v->nickname.value, v->id, msg.value);
			if (!server_cmd_handle(v->server, server_cmd_parse(&msg), v, &msg))
				server_broadcast_ex(v->server, packet, true, v->id);

			break;
		}
	}

	return true;
}
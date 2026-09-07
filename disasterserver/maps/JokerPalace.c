#include <maps/JokerPalace.h>
#include <entities/SpkieController.h>
#include <entities/JPDoor.h>
#include <entities/JPShard.h>
#include <States.h>
#include <CMath.h>
#include <stdlib.h>
#include <string.h>

#define JP_GAMBLE_RING_COST 3
#define JP_GAMBLE_SHARD_COST 1
#define JP_GAMBLE_UNLOCK_TIME (2 * TICKSPERSEC - 30)
#define JP_GAMBLE_SAFE_ROLLS 6
#define JP_GAMBLE_GUARANTEE_ROLL 11
#define JP_SHARD_COUNT 64

enum
{
	JP_GAMBLE_NOTHING,
	JP_GAMBLE_FIVE_RINGS,
	JP_GAMBLE_RED_RING,
	JP_GAMBLE_SPEED,
	JP_GAMBLE_SLOW,
	JP_GAMBLE_DAMAGE,
	JP_GAMBLE_JACKPOT
};

static bool jp_machine_pos(uint16_t x, uint16_t y)
{
	const Vector2 machines[] =
	{
		{ 33.0f, 794.0f },
		{ 193.0f, 306.0292f },
		{ 1145.0f, 194.0f },
		{ 1478.0f, 66.0f },
		{ 1865.0f, 722.0f },
	};
	Vector2 pos = { (float)x, (float)y };

	for (size_t i = 0; i < sizeof(machines) / sizeof(machines[0]); i++)
	{
		Vector2 machine = machines[i];
		if (vector2_dist(&pos, &machine) <= 16.0f)
			return true;
	}

	return false;
}

static uint8_t jp_roll_result(Server* server)
{
	if (!server->game.jp_gamble_opened && server->game.jp_gamble_failed_rolls + 1 >= JP_GAMBLE_GUARANTEE_ROLL)
		return JP_GAMBLE_JACKPOT;

	if (!server->game.jp_gamble_opened && server->game.jp_gamble_failed_rolls >= JP_GAMBLE_SAFE_ROLLS && (rand() % 100) < 12)
		return JP_GAMBLE_JACKPOT;

	uint8_t roll = rand() % 100;
	if (roll < 20)
		return JP_GAMBLE_NOTHING;
	if (roll < 45)
		return JP_GAMBLE_FIVE_RINGS;
	if (roll < 58)
		return JP_GAMBLE_RED_RING;
	if (roll < 73)
		return JP_GAMBLE_SPEED;
	if (roll < 88)
		return JP_GAMBLE_SLOW;

	return JP_GAMBLE_DAMAGE;
}

static JPShard* jp_find_shard(Server* server, uint16_t eid)
{
	for (size_t i = 0; i < server->game.entities.capacity; i++)
	{
		Entity* entity = (Entity*)server->game.entities.ptr[i];
		if (!entity)
			continue;

		if (entity->id == eid && strcmp(entity->tag, "jpshard") == 0)
			return (JPShard*)entity;
	}

	return NULL;
}

static bool jp_open(Server* server)
{
	if (server->game.jp_gamble_opened)
		return true;

	server->game.jp_gamble_opened = true;
	for (size_t i = 0; i < sizeof(server->game.jp_door_ids) / sizeof(server->game.jp_door_ids[0]); i++)
	{
		if (server->game.jp_door_ids[i])
			RAssert(game_despawn(server, NULL, server->game.jp_door_ids[i]));

		server->game.jp_door_ids[i] = 0;
	}

	Packet pack;
	PacketCreate(&pack, SERVER_HDDOOR_STATE);
	PacketWrite(&pack, packet_write8, 0);
	PacketWrite(&pack, packet_write8, 1);
	server_broadcast(server, &pack, true);

	return true;
}

bool jp_init(Server* server)
{
	RAssert(map_time(server, 3.42 * TICKSPERSEC, 20)); //205
	RAssert(map_ring(server, 5));
	RAssert(game_spawn(server, (Entity*)&(MakeSpike()), sizeof(SpikeController), NULL));
	server->game.jp_gamble_failed_rolls = 0;
	server->game.jp_gamble_opened = false;
	server->game.jp_door_ids[0] = 0;
	server->game.jp_door_ids[1] = 0;

	Entity* door = NULL;
	RAssert(game_spawn(server, (Entity*)&(MakeJPDoor(927, 768, 0)), sizeof(JPDoor), &door));
	server->game.jp_door_ids[0] = door->id;
	RAssert(game_spawn(server, (Entity*)&(MakeJPDoor(1279, 768, 1)), sizeof(JPDoor), &door));
	server->game.jp_door_ids[1] = door->id;

	for (uint8_t i = 0; i < JP_SHARD_COUNT; i++)
		RAssert(game_spawn(server, (Entity*)&(MakeJPShard(i)), sizeof(JPShard), NULL));

	return true;
}

bool jp_tick(Server* server)
{
	if (server->game.time_sec <= TICKSPERSEC - 30 && server->game.bring_state < BS_ACTIVATED)
	{
		if (server->game.bring_state < BS_DEACTIVATED)
			RAssert(game_bigring(server, BS_DEACTIVATED));

		RAssert(game_bigring(server, BS_ACTIVATED));
	}

	return true;
}

bool jp_tcpmsg(PeerData* v, Packet* packet)
{
	PacketRead(passtrough, packet, packet_read8, uint8_t);
	PacketRead(type, packet, packet_read8, uint8_t);

	switch (type)
	{
		case CLIENT_JPGAMBLE_ROLL:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, v->id != v->server->game.exe);

			if (v->server->game.end > 0)
				break;

			if (v->server->game.time_sec > JP_GAMBLE_UNLOCK_TIME)
				break;

			if (v->server->game.jp_gamble_opened)
				break;

			if (v->plr.flags & (PLAYER_DEAD | PLAYER_DEMONIZED | PLAYER_ESCAPED))
				break;

			if (v->plr.data[0] < JP_GAMBLE_SHARD_COST && v->plr.rings < JP_GAMBLE_RING_COST)
				break;

			PacketRead(x, packet, packet_read16, uint16_t);
			PacketRead(y, packet, packet_read16, uint16_t);
			AssertOrDisconnect(v->server, jp_machine_pos(x, y));

			Vector2 pos = { (float)x, (float)y };
			AssertOrDisconnect(v->server, vector2_dist(&pos, &v->plr.pos) <= 96.0f);

			if (v->plr.data[0] >= JP_GAMBLE_SHARD_COST)
			{
				v->plr.data[0] -= JP_GAMBLE_SHARD_COST;
			}
			else
			{
				v->plr.rings -= JP_GAMBLE_RING_COST;
				time_start(&v->plr.last_rings);
			}

			uint8_t result = jp_roll_result(v->server);

			if (result == JP_GAMBLE_JACKPOT)
			{
				RAssert(jp_open(v->server));
			}
			else
			{
				v->server->game.jp_gamble_failed_rolls++;
			}

			if (result == JP_GAMBLE_FIVE_RINGS)
			{
				time_start(&v->plr.last_rings);
				v->plr.rings += 5;
				v->plr.stats.rings += 5;
			}

			Packet pack;
			PacketCreate(&pack, SERVER_JPGAMBLE_STATE);
			PacketWrite(&pack, packet_write16, v->id);
			PacketWrite(&pack, packet_write16, x);
			PacketWrite(&pack, packet_write16, y);
			PacketWrite(&pack, packet_write8, result);
			PacketWrite(&pack, packet_write16, v->plr.rings);
			PacketWrite(&pack, packet_write8, (uint8_t)v->plr.data[0]);
			PacketWrite(&pack, packet_write8, v->server->game.jp_gamble_opened);
			PacketWrite(&pack, packet_write8, v->server->game.jp_gamble_failed_rolls);
			server_broadcast(v->server, &pack, true);
			break;
		}

		case CLIENT_JPSHARD_COLLECT:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, v->id != v->server->game.exe);

			if (v->server->game.end > 0)
				break;

			if (v->plr.flags & (PLAYER_DEAD | PLAYER_DEMONIZED | PLAYER_ESCAPED))
				break;

			PacketRead(rid, packet, packet_read8, uint8_t);
			PacketRead(eid, packet, packet_read16, uint16_t);

			JPShard* shard = jp_find_shard(v->server, eid);
			if (!shard || shard->rid != rid)
				break;

			JPShard* ent = NULL;
			if (!game_despawn(v->server, (Entity**)&ent, eid))
				break;

			v->plr.data[0]++;

			Packet pack;
			PacketCreate(&pack, SERVER_JPSHARD_STATE);
			PacketWrite(&pack, packet_write8, 1);
			PacketWrite(&pack, packet_write8, ent->rid);
			PacketWrite(&pack, packet_write16, ent->id);
			PacketWrite(&pack, packet_write16, v->id);
			PacketWrite(&pack, packet_write8, (uint8_t)v->plr.data[0]);
			server_broadcast(v->server, &pack, true);

			free(ent);
			break;
		}
	}

	return true;
}

// resend the current one-shot entity states to a peer that joined mid-match (spectator)
bool jp_sync(Server* server, PeerData* v)
{
	for (size_t i = 0; i < server->game.entities.capacity; i++)
	{
		Entity* ent = (Entity*)server->game.entities.ptr[i];
		if (!ent)
			continue;

		Packet pack;

		if (strcmp(ent->tag, "jpshard") == 0)
		{
			JPShard* shard = (JPShard*)ent;

			PacketCreate(&pack, SERVER_JPSHARD_STATE);
			PacketWrite(&pack, packet_write8, 0);
			PacketWrite(&pack, packet_write8, shard->rid);
			PacketWrite(&pack, packet_write16, shard->id);
			RAssert(packet_send(v->peer, &pack, true));
		}
		else if (strcmp(ent->tag, "jpdoor") == 0)
		{
			JPDoor* door = (JPDoor*)ent;

			PacketCreate(&pack, SERVER_JPDOOR_STATE);
			PacketWrite(&pack, packet_write8, 0);
			PacketWrite(&pack, packet_write16, door->id);
			PacketWrite(&pack, packet_write16, (uint16_t)door->pos.x);
			PacketWrite(&pack, packet_write16, (uint16_t)door->pos.y);
			PacketWrite(&pack, packet_write8, door->slot);
			RAssert(packet_send(v->peer, &pack, true));
		}
	}

	// doors that already opened are despawned server-side; still show them, opened
	if (server->game.jp_gamble_opened)
	{
		for (uint8_t slot = 0; slot < 2; slot++)
		{
			uint16_t id = 9000 + slot;

			Packet pack;
			PacketCreate(&pack, SERVER_JPDOOR_STATE);
			PacketWrite(&pack, packet_write8, 0);
			PacketWrite(&pack, packet_write16, id);
			PacketWrite(&pack, packet_write16, (uint16_t)(slot == 0 ? 927 : 1279));
			PacketWrite(&pack, packet_write16, 768);
			PacketWrite(&pack, packet_write8, slot);
			RAssert(packet_send(v->peer, &pack, true));

			PacketCreate(&pack, SERVER_JPDOOR_STATE);
			PacketWrite(&pack, packet_write8, 1);
			PacketWrite(&pack, packet_write16, id);
			RAssert(packet_send(v->peer, &pack, true));
		}
	}

	return true;
}

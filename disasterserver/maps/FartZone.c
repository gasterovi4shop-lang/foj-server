#include <maps/FartZone.h>
#include <entities/SpkieController.h>
#include <entities/DTBall.h>
#include <entities/BlackRing.h>
#include <entities/Dummy.h>
#include <States.h>
#include <CMath.h>
#include <stdlib.h>

#define JP_GAMBLE_RING_COST 3
#define JP_GAMBLE_SHARD_COST 1

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

//позиции автоматов в room_fartzone
static bool ft_machine_pos(uint16_t x, uint16_t y)
{
	const Vector2 machines[] =
	{
		{ 623.0f, 2434.0f },
		{ 687.0f, 2434.0f },
		{ 751.0f, 2434.0f },
	};
	Vector2 pos = { (float)x, (float)y };

	for (size_t i = 0; i < sizeof(machines) / sizeof(machines[0]); i++)
	{
		if (vector2_dist(&pos, &machines[i]) <= 16.0f)
			return true;
	}

	return false;
}

//та же таблица шансов, что на Joker's Palace. Джекпот — полное лечение (применяет
//клиент); зависимости открытия дверей на Fart Zone нет
static uint8_t ft_roll_result(void)
{
	uint8_t roll = rand() % 100;
	if (roll < 30)
		return JP_GAMBLE_NOTHING;    //30%
	if (roll < 40)
		return JP_GAMBLE_FIVE_RINGS; //10%
	if (roll < 52)
		return JP_GAMBLE_RED_RING;   //12%
	if (roll < 63)
		return JP_GAMBLE_SPEED;      //11%
	if (roll < 75)
		return JP_GAMBLE_SLOW;       //12%
	if (roll < 90)
		return JP_GAMBLE_DAMAGE;     //15%

	return JP_GAMBLE_JACKPOT;        //10%
}

bool ft_init(Server* server)
{
	RAssert(map_time(server, 4.27 * TICKSPERSEC, 20));
	RAssert(map_ring(server, 1));

	for (uint8_t i = 0; i < 3; i++)
		RAssert(game_spawn(server, (Entity*)&(MakeBlackRing(MAP_BRING, MAP_BRING)), sizeof(BRing), NULL));

	RAssert(game_spawn(server, (Entity*)&(MakeDummy()), sizeof(Dummy), NULL));
	RAssert(game_spawn(server, (Entity*)&(MakeSpike()), sizeof(SpikeController), NULL));
	RAssert(game_spawn(server, (Entity*)&(MakeDTBall()), sizeof(DTBall), NULL));

	return true;
}

bool ft_tcpmsg(PeerData* v, Packet* packet)
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

			if (v->plr.flags & (PLAYER_DEAD | PLAYER_DEMONIZED | PLAYER_ESCAPED))
				break;

			if (v->plr.data[0] < JP_GAMBLE_SHARD_COST && v->plr.rings < JP_GAMBLE_RING_COST)
				break;

			PacketRead(x, packet, packet_read16, uint16_t);
			PacketRead(y, packet, packet_read16, uint16_t);
			AssertOrDisconnect(v->server, ft_machine_pos(x, y));

			Vector2 pos = { (float)x, (float)y };
			AssertOrDisconnect(v->server, vector2_dist(&pos, &v->plr.pos) <= 96.0f);

			//на Fart Zone фишек нет, data[0] всегда 0 — плата идёт кольцами
			if (v->plr.data[0] >= JP_GAMBLE_SHARD_COST)
			{
				v->plr.data[0] -= JP_GAMBLE_SHARD_COST;
			}
			else
			{
				v->plr.rings -= JP_GAMBLE_RING_COST;
				time_start(&v->plr.last_rings);
			}

			uint8_t result = ft_roll_result();

			if (result == JP_GAMBLE_FIVE_RINGS)
			{
				time_start(&v->plr.last_rings);
				v->plr.rings += 5;
				v->plr.stats.rings += 5;
			}

			//формат пакета тот же, что на Joker's Palace; дверей на карте нет,
			//поэтому opened = false и fails = 0
			Packet pack;
			PacketCreate(&pack, SERVER_JPGAMBLE_STATE);
			PacketWrite(&pack, packet_write16, v->id);
			PacketWrite(&pack, packet_write16, x);
			PacketWrite(&pack, packet_write16, y);
			PacketWrite(&pack, packet_write8, result);
			PacketWrite(&pack, packet_write16, v->plr.rings);
			PacketWrite(&pack, packet_write8, (uint8_t)v->plr.data[0]);
			PacketWrite(&pack, packet_write8, false);
			PacketWrite(&pack, packet_write8, 0);
			server_broadcast(v->server, &pack, true);
			break;
		}

		case CLIENT_FART_PUSH:
		{
			AssertOrDisconnect(v->server, v->in_game);
			PacketRead(spd, packet, packet_read8, int8_t);

			Dummy* dum;
			if (!game_find(v->server, (Entity**)&dum, "dummy", 1))
				break;

			dummy_activate(dum, spd);
			break;
		}
	}

	return true;
}

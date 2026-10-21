#ifndef JPSHARD_H
#define JPSHARD_H
#include "../States.h"

bool jpshard_init(Server* server, Entity* entity);
bool jpshard_uninit(Server* server, Entity* entity);

typedef struct
{
	ENTITY_BODY

	uint8_t rid;
	// loose shard: dropped on the ground by a dying player instead of a spawner;
	// pid is who dropped it, gen matches the dropper's plr.data[1] drop counter
	bool dropped;
	uint16_t pid;
	uint16_t gen;
} JPShard;

// rid of a loose shard: never matches a spawner shard (0..JP_SHARD_COUNT-1)
#define JP_SHARD_LOOSE_RID 0xFF

#define MakeJPShard(rid) ((JPShard) { MakeEntity("jpshard", 0, 0) jpshard_init, NULL, jpshard_uninit, rid, false, 0, 0 })

#endif

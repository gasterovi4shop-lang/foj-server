#ifndef JPSHARD_H
#define JPSHARD_H
#include "../States.h"

bool jpshard_init(Server* server, Entity* entity);
bool jpshard_uninit(Server* server, Entity* entity);

typedef struct
{
	ENTITY_BODY

	uint8_t rid;
} JPShard;

#define MakeJPShard(rid) ((JPShard) { MakeEntity("jpshard", 0, 0) jpshard_init, NULL, jpshard_uninit, rid })

#endif

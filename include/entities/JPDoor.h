#ifndef JPDOOR_H
#define JPDOOR_H
#include "../States.h"

bool jpdoor_init(Server* server, Entity* entity);
bool jpdoor_uninit(Server* server, Entity* entity);

typedef struct
{
	ENTITY_BODY

	uint8_t slot;
} JPDoor;

#define MakeJPDoor(x, y, slot) ((JPDoor) { MakeEntity("jpdoor", x, y) jpdoor_init, NULL, jpdoor_uninit, slot })

#endif

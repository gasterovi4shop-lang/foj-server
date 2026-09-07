#include <entities/JPDoor.h>

bool jpdoor_init(Server* server, Entity* entity)
{
	JPDoor* door = (JPDoor*)entity;

	Packet pack;
	PacketCreate(&pack, SERVER_JPDOOR_STATE);
	PacketWrite(&pack, packet_write8, 0);
	PacketWrite(&pack, packet_write16, door->id);
	PacketWrite(&pack, packet_write16, (uint16_t)door->pos.x);
	PacketWrite(&pack, packet_write16, (uint16_t)door->pos.y);
	PacketWrite(&pack, packet_write8, door->slot);
	server_broadcast(server, &pack, true);

	return true;
}

bool jpdoor_uninit(Server* server, Entity* entity)
{
	Packet pack;
	PacketCreate(&pack, SERVER_JPDOOR_STATE);
	PacketWrite(&pack, packet_write8, 1);
	PacketWrite(&pack, packet_write16, entity->id);
	server_broadcast(server, &pack, true);

	return true;
}

#include <entities/JPShard.h>

bool jpshard_init(Server* server, Entity* entity)
{
	JPShard* shard = (JPShard*)entity;

	Packet pack;
	PacketCreate(&pack, SERVER_JPSHARD_STATE);
	PacketWrite(&pack, packet_write8, 0);
	PacketWrite(&pack, packet_write8, shard->rid);
	PacketWrite(&pack, packet_write16, shard->id);
	server_broadcast(server, &pack, true);

	return true;
}

bool jpshard_uninit(Server* server, Entity* entity)
{
	(void)server;
	(void)entity;

	return true;
}

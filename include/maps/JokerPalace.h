#ifndef PALACE_H
#define PALACE_H
#include "../Maps.h"

bool jp_init(Server* server);
bool jp_tick(Server* server);
bool jp_tcpmsg(PeerData* v, Packet* packet);

// resend one-shot entity states (shards, doors) to a late spectator
bool jp_sync(Server* server, PeerData* v);

#endif

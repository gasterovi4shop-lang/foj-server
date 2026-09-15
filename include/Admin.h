#ifndef ADMIN_H
#define ADMIN_H

#include <Api.h>
#include <stddef.h>
#include <stdbool.h>

// Admin control API: a plain-TCP JSON-lines listener (g_config.admin_port)
// that lets an external client - e.g. the Telegram bot - list players,
// read per-lobby online counts and execute console commands remotely.
//
// Protocol: one JSON request line in, one JSON response line out.
//   {"cmd":"status"}                      -> {"ok":true,"servers":[...]}
//   {"cmd":"players","server":0}          -> {"ok":true,"players":[...]}
//   {"cmd":"cmds"}                        -> {"ok":true,"cmds":[...]}
//   {"cmd":"cmdlog"}                      -> {"ok":true,"log":[...]}
//   {"cmd":"chat_lobbies"}                -> {"lobbies":[{"id":0,"online":0,"state":"lobby"}]}
//   {"cmd":"chatlog","server":0}          -> {"messages":[...]}
//   {"cmd":"joins"}                       -> {"ok":true,"joins":[...]}
//   {"cmd":"known","offset":0,"limit":100}
//                                         -> {"players":[{"id":"...","nick":"..."}]}
//   {"cmd":"exec","server":0,"line":".ban x 1h griefing"}
//                                         -> {"ok":true,"output":"..."}
SERVER_API bool admin_init(void);

// records every client chat line, including dot commands, for the admin API
SERVER_API void admin_log_chat(int server, const char* nick, const char* accid,
	int player_id, const char* text);

// called by the server when a player passes all checks and joins a lobby;
// kept in a ring buffer, exposed through the "joins" admin command
SERVER_API void admin_log_join(const char* nick, const char* accid, const char* udid, int id);

// accumulates in-game playtime for an account id (called from the tick loop)
SERVER_API void admin_add_playtime(const char* accid, const char* nick, double seconds);

// checks whether an account id was ever issued (used by the bot login)
SERVER_API bool admin_profile(const char* accid, char* nick, size_t nickcap, double* seconds);

#endif

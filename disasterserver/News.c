#include <Config.h>
#include <Log.h>
#include <io/Threads.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <unistd.h>
#endif

// The "новостник" (news server): a tiny plain-TCP listener that runs next to the
// game servers. Clients connect once per launch, ask for the notification list,
// download it and disconnect. The admin pushes notifications from the panel UI.
//
// Protocol (all responses end with '\n'):
//   client: "GETNEWS\n"
//   server: "NEWS <count> <epoch>\n"
//           <one cJSON line per notification, newest first>\n
//           "END\n"
//   anything else: "ERR\n"

#define NEWS_MAX_ITEMS	32
#define NEWS_MAX_TEXT	2048

static cJSON*	g_news = NULL;
static Mutex	g_newsMut;

// MutexLock/MutexUnlock expand to RAssert (which does "return false"), so they
// can't be used directly inside void thread callbacks (MSVC C4098)
static bool news_lock(void)
{
	MutexLock(g_newsMut);
	return true;
}

static bool news_unlock(void)
{
	MutexUnlock(g_newsMut);
	return true;
}

static bool send_all(int sock, const char* data, size_t len)
{
	size_t sent = 0;
	while (sent < len)
	{
#ifdef _WIN32
		int n = send(sock, data + sent, (int)(len - sent), 0);
#else
		ssize_t n = send(sock, data + sent, len - sent, 0);
#endif
		if (n <= 0)
			return false;

		sent += (size_t)n;
	}

	return true;
}

// Reads a '\n'-terminated line byte by byte. Returns false on error or overflow.
static bool recv_line(int sock, char* out, size_t cap)
{
	size_t len = 0;
	while (len + 1 < cap)
	{
		char c;
#ifdef _WIN32
		int n = recv(sock, &c, 1, 0);
#else
		ssize_t n = recv(sock, &c, 1, 0);
#endif
		if (n <= 0)
			return false;

		if (c == '\n')
		{
			out[len] = '\0';
			return true;
		}

		if (c != '\r')
			out[len++] = c;
	}

	return false;
}

static void news_client_thread(void* arg)
{
	int sock = (int)(intptr_t)arg;
	char line[64];

	if (!recv_line(sock, line, sizeof(line)))
	{
		Info("News: client disconnected before sending a request.");
		goto done;
	}

	Info("News: client request \"%s\"", line);

	if (strncmp(line, "GETNEWS", 7) == 0)
	{
		news_lock();
		{
			cJSON* items = cJSON_GetObjectItemCaseSensitive(g_news, "items");
			int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;

			//эпоха базы: меняется при пересоздании News.json. Клиент по ней
			//понимает, что id пошли заново, и тихо перенимает текущие записи
			cJSON* epochjs = cJSON_GetObjectItemCaseSensitive(g_news, "epoch");
			uint32_t epoch = cJSON_IsNumber(epochjs) ? (uint32_t)epochjs->valuedouble : 0;

			// весь ответ собирается в один буфер и уходит ОДНИМ send():
			// GameMaker стабильно обрабатывает один data-event на пакет,
			// а россыпь мелких send-ов по реальной сети терялась
			size_t cap = 160 + (size_t)count * 2200;
			char* resp = (char*)malloc(cap);

			if (!resp)
			{
				news_unlock();
				goto done;
			}

			size_t ro = (size_t)snprintf(resp, cap, "NEWS %d %u\n", count, epoch);

			cJSON* item = NULL;
			cJSON_ArrayForEach(item, items)
			{
				cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
				const char* text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "text"));
				if (!cJSON_IsNumber(id) || !text)
					continue;

				//каждое уведомление - одна json-строка (текст без переносов)
				char* line = cJSON_PrintUnformatted(item);
				if (!line)
					continue;

				if (ro + strlen(line) + 2 < cap)
				{
					memcpy(resp + ro, line, strlen(line));
					ro += strlen(line);
					resp[ro++] = '\n';
				}

				free(line);
			}

			if (ro + 4 < cap)
			{
				memcpy(resp + ro, "END\n", 4);
				ro += 4;
			}

			send_all(sock, resp, ro);
			free(resp);
			Info("News: response sent (%d items, %zu bytes)", count, ro);
		}
		news_unlock();
	}
	else
		send_all(sock, "ERR\n", 4);

	// Полу-закрытие: клиенты могут досылать лишние байты после строки запроса
	// (например GameMaker пишет NUL-терминатор после '\n'). Если закрыть сокет
	// с непрочитанными данными в приёмном буфере, ядро отправит RST и клиент
	// потеряет уже отправленный ответ.
#ifdef _WIN32
	{
		DWORD rcv_to = 2000;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_to, sizeof(rcv_to));
		shutdown(sock, SD_SEND);
	}
#else
	{
		struct timeval tv = { 2, 0 };
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		shutdown(sock, SHUT_WR);
	}
#endif

	{
		char drain;
		while (recv(sock, &drain, 1, 0) > 0) {}
	}

done:
#ifdef _WIN32
	closesocket(sock);
#else
	close(sock);
#endif
}

static void news_listen_thread(void* arg)
{
	(void)arg;

	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
	{
		Err("News server: failed to create socket.");
		return;
	}

#ifdef _WIN32
	BOOL reuse = TRUE;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
	int reuse = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)g_config.news_port);

	if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		Err("News server: failed to bind port %d.", g_config.news_port);
#ifdef _WIN32
		closesocket(listener);
#else
		close(listener);
#endif
		return;
	}

	if (listen(listener, 8) < 0)
	{
		Err("News server: failed to listen on port %d.", g_config.news_port);
#ifdef _WIN32
		closesocket(listener);
#else
		close(listener);
#endif
		return;
	}

	Info("News server listening on port %d.", g_config.news_port);

	for (;;)
	{
		int client = (int)accept(listener, NULL, NULL);
		if (client < 0)
			continue;

		// one throwaway thread per client: news clients are short-lived
		Thread th;
		ThreadSpawn(th, news_client_thread, (void*)(intptr_t)client);
	}
}

SERVER_API bool news_init(void)
{
	MutexCreate(g_newsMut);

	if (!collection_init(&g_news, NEWS_FILE, "{\"next_id\":1,\"items\":[]}"))
		return false;

	if (!cJSON_IsObject(g_news))
	{
		cJSON_Delete(g_news);
		g_news = cJSON_CreateObject();
	}

	bool metadata_changed = false;
	cJSON* next = cJSON_GetObjectItemCaseSensitive(g_news, "next_id");
	if (!cJSON_IsNumber(next))
	{
		cJSON_AddNumberToObject(g_news, "next_id", 1);
		metadata_changed = true;
	}

	// The epoch is stable for the lifetime of this News.json.  A new value
	// lets clients distinguish a recreated database from newly added entries.
	cJSON* epoch = cJSON_GetObjectItemCaseSensitive(g_news, "epoch");
	if (!cJSON_IsNumber(epoch) || epoch->valuedouble <= 0)
	{
		uint32_t value = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)g_news;
		if (value == 0)
			value = 1;

		if (epoch)
			cJSON_SetNumberValue(epoch, (double)value);
		else
			cJSON_AddNumberToObject(g_news, "epoch", (double)value);

		metadata_changed = true;
	}

	cJSON* items = cJSON_GetObjectItemCaseSensitive(g_news, "items");
	if (!cJSON_IsArray(items))
	{
		cJSON_AddItemToObject(g_news, "items", cJSON_CreateArray());
		metadata_changed = true;
	}

	if (metadata_changed)
		collection_save(NEWS_FILE, g_news);

	if (g_config.news_enabled)
	{
		Thread th;
		ThreadSpawn(th, news_listen_thread, NULL);
	}

	return true;
}

SERVER_API bool news_push(const char* text)
{
	if (!text)
		return false;

	// strip control characters and cap the text; only complete utf8
	// sequences are kept (a cut-off tail would render as garbage)
	char clean[NEWS_MAX_TEXT + 1];
	size_t len = 0;

	for (const char* c = text; *c; )
	{
		unsigned char b = (unsigned char)*c;

		// "\n" typed as two chars becomes a real line break
		if (b == '\\' && c[1] == 'n')
		{
			c += 2;
			if (len < NEWS_MAX_TEXT)
				clean[len++] = '\n';
			continue;
		}

		int seq = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;

		if (seq == 1 && (b < 0x20 || b == 0x7F) && b != '\n')
		{
			c++;
			if (len < NEWS_MAX_TEXT)
				clean[len++] = ' ';
			continue;
		}

		bool complete = true;
		for (int i = 1; i < seq; i++)
		{
			if (((unsigned char)c[i] & 0xC0) != 0x80)
			{
				complete = false;
				break;
			}
		}

		if (!complete || len + seq > NEWS_MAX_TEXT)
			break;

		for (int i = 0; i < seq; i++)
			clean[len++] = c[i];

		c += seq;
	}

	clean[len] = '\0';

	if (clean[0] == '\0' || strspn(clean, " ") == strlen(clean))
	{
		Warn("News: empty notification, not sent.");
		return false;
	}

	bool res = true;
	MutexLock(g_newsMut);
	{
		cJSON* next = cJSON_GetObjectItemCaseSensitive(g_news, "next_id");
		uint32_t id = next ? (uint32_t)next->valuedouble : 1;

		if (next)
			cJSON_SetNumberHelper(next, (double)id + 1);

		cJSON* items = cJSON_GetObjectItemCaseSensitive(g_news, "items");

		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "id", (double)id);
		cJSON_AddStringToObject(item, "text", clean);
		cJSON_InsertItemInArray(items, 0, item);

		// drop the oldest entries beyond the cap
		while (cJSON_GetArraySize(items) > NEWS_MAX_ITEMS)
			cJSON_DeleteItemFromArray(items, cJSON_GetArraySize(items) - 1);

		res = collection_save(NEWS_FILE, g_news);
	}
	MutexUnlock(g_newsMut);

	if (res)
		Info("News: sent notification \"%s\"", clean);

	return res;
}

SERVER_API size_t news_count(void)
{
	size_t count = 0;

	MutexLock(g_newsMut);
	{
		cJSON* items = cJSON_GetObjectItemCaseSensitive(g_news, "items");
		if (cJSON_IsArray(items))
			count = (size_t)cJSON_GetArraySize(items);
	}
	MutexUnlock(g_newsMut);

	return count;
}

SERVER_API bool news_get(size_t index, uint32_t* id, char* out, size_t cap)
{
	bool res = false;

	MutexLock(g_newsMut);
	{
		cJSON* items = cJSON_GetObjectItemCaseSensitive(g_news, "items");
		cJSON* item = cJSON_IsArray(items) ? cJSON_GetArrayItem(items, (int)index) : NULL;
		if (item)
		{
			cJSON* jsid = cJSON_GetObjectItemCaseSensitive(item, "id");
			const char* text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "text"));

			if (jsid && text)
			{
				*id = (uint32_t)jsid->valuedouble;
				snprintf(out, cap, "%s", text);
				res = true;
			}
		}
	}
	MutexUnlock(g_newsMut);

	return res;
}

SERVER_API bool news_delete(uint32_t id)
{
	bool res = false;

	MutexLock(g_newsMut);
	{
		cJSON* items = cJSON_GetObjectItemCaseSensitive(g_news, "items");
		int idx = 0;

		cJSON* item = NULL;
		cJSON_ArrayForEach(item, items)
		{
			cJSON* jsid = cJSON_GetObjectItemCaseSensitive(item, "id");
			if (cJSON_IsNumber(jsid) && (uint32_t)jsid->valuedouble == id)
			{
				cJSON_DeleteItemFromArray(items, idx);
				res = collection_save(NEWS_FILE, g_news);
				break;
			}

			idx++;
		}
	}
	MutexUnlock(g_newsMut);

	if (res)
		Info("News: deleted notification %u", id);

	return res;
}

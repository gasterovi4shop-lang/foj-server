#ifndef COMPONENTS_H
#define COMPONENTS_H
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <Maps.h>
#include <ui/Resources.h>

#define COMPONENT_BODY int x, y, w, h; UpdateCallback update
#define COLOR_WHITE (SDL_Color)	{ 255, 255, 255, 255 }
#define COLOR_RED	(SDL_Color) { 194, 0, 55, 255 }
#define COLOR_GRN	(SDL_Color) { 15, 255, 57, 255 }
#define COLOR_PUR	(SDL_Color) { 184, 36, 255, 255 }
#define COLOR_BLU	(SDL_Color) { 93, 103, 255, 255 }
#define COLOR_GRA	(SDL_Color) { 100, 100, 100, 255 }
#define COLOR_YLW	(SDL_Color) { 255, 219, 0, 255 }
#define COLOR_ORG	(SDL_Color) { 234, 96, 20, 255 }
#define INTERFACE_SCALE 2

extern int g_mouseWheel;

struct _Component;
typedef bool (*UpdateCallback)(SDL_Renderer*, struct _Component*);
typedef struct _Component
{
	COMPONENT_BODY;
} Component;

typedef struct
{
	COMPONENT_BODY;
	const char* text;
	int scale;
} Label;
#define LabelCreate(x, y, text, scale) (Label) { x, y, 0, 0, label_update, text, scale }
bool label_update(SDL_Renderer* renderer, struct _Component* component);

typedef struct
{
	COMPONENT_BODY;
	int d_x, d_y, d_w, d_h;
} Image;
#define ImageCreate(x, y, w, h, s_x, s_y, s_w, s_h) (Image) { s_x, s_y, s_w, s_h, image_update, x, y, w, h }
bool image_update(SDL_Renderer* renderer, struct _Component* component);

typedef bool (*ButtonCallback)(struct _Component* component);
typedef struct
{
	COMPONENT_BODY;
	int d_x, d_y, d_w, d_h;
	ButtonCallback cb;
	bool clicked;
} Button;
#define ButtonCreate(x, y, w, h, cb, s_x, s_y, s_w, s_h) (Button) { s_x, s_y, s_w, s_h, button_update, x, y, w, h, cb, false }
bool button_update(SDL_Renderer* renderer, struct _Component* component);

typedef struct
{
	COMPONENT_BODY;
	int d_x, d_y, d_w, d_h;
	ButtonCallback cb;
	bool clicked, reverse;
	bool* value;
} ToggleButton;
#define TButtonCreate(x, y, w, h, cb, pointer, reverse, s_x, s_y, s_w, s_h) (ToggleButton) { s_x, s_y, s_w, s_h, tbutton_update, x, y, w, h, cb, false, reverse, pointer }
bool tbutton_update(SDL_Renderer* renderer, struct _Component* component);

typedef struct
{
	COMPONENT_BODY;
	int d_x, d_y, d_w, d_h;
	ButtonCallback cb;
	bool clicked;
	PeerData peer;
} PlayerButton;

typedef struct
{
	COMPONENT_BODY;
	bool clicked;
	ButtonCallback cb;
	float scroll, target_scroll;
} MapList;
#define MapListCreate(x, y, w, h, cb) (MapList) { x, y, w, h, maplist_update, false, cb, 0, 0 }
bool maplist_update(SDL_Renderer* renderer, struct _Component* component);

typedef struct
{
	COMPONENT_BODY;
	bool clicked;
	int preset;
	Label label;
} MapListPreset;
#define MapListPresetCreate(x, y, w, h) (MapListPreset) { x, y, w, h, mappreset_update, false, 0 }
bool mappreset_update(SDL_Renderer* renderer, struct _Component* component);

typedef struct
{
	COMPONENT_BODY;
	bool clicked;
	int preset;
	Label label;
} PingLimit;
#define PingLimitCreate(x, y, w, h) (PingLimit) { x, y, w, h, ping_update, false, 0 }
bool ping_update(SDL_Renderer* renderer, struct _Component* component);

typedef struct
{
	COMPONENT_BODY;
	bool clicked;
	PeerData peers[7];
} PlayerList;

#define PlayerListCreate(x, y) (PlayerList) { x, y, 144, 176, playerlist_update, false, {0} }
bool playerlist_update(SDL_Renderer* renderer, struct _Component* component);

typedef struct
{
	COMPONENT_BODY;
	int d_x, d_y, d_w, d_h;
	ButtonCallback cb;
	bool clicked;

	const char* collection;
	cJSON* root;
	const char* key;
} DeleteButton;

typedef struct
{
	COMPONENT_BODY;
	bool clicked;
	int page;
} PlayerListConfig;
#define PlayerListConfigCreate(x, y, update) (PlayerListConfig) { x, y, 144, 176, update, false, 0 }
bool playerlist_bans_update(SDL_Renderer* renderer, struct _Component* component);
bool playerlist_op_update(SDL_Renderer* renderer, struct _Component* component);

// news tab: single-line text input for the notification text
typedef struct
{
	COMPONENT_BODY;
	bool clicked;
} NewsTextInput;
#define NewsTextInputCreate(x, y, w, h) (NewsTextInput) { x, y, w, h, newstext_update, false }
bool newstext_update(SDL_Renderer* renderer, struct _Component* component);

// news tab: centered transient status line ("sent!" etc)
typedef struct
{
	COMPONENT_BODY;
} NewsStatus;
#define NewsStatusCreate() (NewsStatus) { 0, 0, 0, 0, newsstatus_update }
bool newsstatus_update(SDL_Renderer* renderer, struct _Component* component);

// news tab: list of recently sent notifications (right-side section)
typedef struct
{
	COMPONENT_BODY;
} NewsList;
#define NewsListCreate(x, y, w, h) (NewsList) { x, y, w, h, newslist_update }
bool newslist_update(SDL_Renderer* renderer, struct _Component* component);

// news tab text entry state (fed from the SDL event loop in ui/Main.c)
void news_text_input(const char* text);
void news_text_key(SDL_Keycode key);
void news_set_status(const char* text);

extern char g_newsText[2048];
extern int g_newsTextLen;
extern char g_newsStatus[64];
extern bool g_newsOpen;

// players tab: generic single-line input for the ban duration and reason
typedef struct
{
	COMPONENT_BODY;
	bool clicked;
	char* buffer;			// куда пишется текст
	int   cap;				// размер буфера
	int   limit;			// максимум символов
	const char* placeholder;
} PanelInput;
#define PanelInputCreate(x, y, w, h, buf, cap, limit, placeholder) (PanelInput) { x, y, w, h, panelinput_update, false, buf, cap, limit, placeholder }
bool panelinput_update(SDL_Renderer* renderer, struct _Component* component);
void panelinput_text(const char* text);
void panelinput_key(SDL_Keycode key);
extern PanelInput* g_panelFocus;

extern char g_banDur[32];
extern char g_banReason[128];

#endif

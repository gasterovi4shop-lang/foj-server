#include <ui/Components.h>
#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>

// активное поле ввода на вкладке Players (или NULL)
PanelInput* g_panelFocus = NULL;

static void panelinput_put(PanelInput* box, const char* text)
{
	int len = (int)strlen(box->buffer);

	for (const char* c = text; *c && len < box->limit && len < box->cap - 1; c++)
	{
		if ((unsigned char)*c >= 0x20 && (unsigned char)*c != 0x7F)
			box->buffer[len++] = *c;
	}

	box->buffer[len] = '\0';
}

void panelinput_text(const char* text)
{
	if (!g_panelFocus)
		return;

	// ctrl+v обрабатывается в panelinput_key
	if (SDL_GetModState() & KMOD_CTRL)
		return;

	panelinput_put(g_panelFocus, text);
}

void panelinput_key(SDL_Keycode key)
{
	if (!g_panelFocus)
		return;

	int len = (int)strlen(g_panelFocus->buffer);

	if (key == SDLK_BACKSPACE && len > 0)
	{
		// бэкспейс по целым utf8-символам
		len--;
		while (len > 0 && ((unsigned char)g_panelFocus->buffer[len] & 0xC0) == 0x80)
			len--;

		g_panelFocus->buffer[len] = '\0';
	}
	else if (key == SDLK_v && (SDL_GetModState() & KMOD_CTRL))
	{
		char* clip = SDL_GetClipboardText();
		if (!clip)
			return;

		panelinput_put(g_panelFocus, clip);
		SDL_free(clip);
	}
	else if (key == SDLK_ESCAPE)
		g_panelFocus = NULL;
}

bool panelinput_update(SDL_Renderer* renderer, struct _Component* component)
{
	PanelInput* box = (PanelInput*)component;
	SDL_Rect dst = { box->x * INTERFACE_SCALE, box->y * INTERFACE_SCALE, box->w * INTERFACE_SCALE, box->h * INTERFACE_SCALE };

	int mouse_x, mouse_y;
	float scale_x, scale_y;
	Uint32 flags = SDL_GetMouseState(&mouse_x, &mouse_y);
	SDL_RenderGetScale(renderer, &scale_x, &scale_y);
	mouse_x = (int)(mouse_x / scale_x);
	mouse_y = (int)(mouse_y / scale_y);

	if (flags & SDL_BUTTON(1))
	{
		bool inside = (mouse_x >= dst.x && mouse_y >= dst.y && mouse_x < dst.x + dst.w && mouse_y < dst.y + dst.h);

		if (inside)
			g_panelFocus = box;
		else if (g_panelFocus == box)
			g_panelFocus = NULL;
	}

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderFillRect(renderer, &dst);

	if (g_panelFocus == box)
		SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
	else
		SDL_SetRenderDrawColor(renderer, 105, 105, 105, 255);

	SDL_RenderDrawRect(renderer, &dst);

	char shown[300];

	if (box->buffer[0] != '\0')
		snprintf(shown, sizeof(shown), "%s", box->buffer);
	else if (g_panelFocus == box)
		snprintf(shown, sizeof(shown), "%s", "|");
	else
		snprintf(shown, sizeof(shown), "%s", box->placeholder);

	if (g_panelFocus == box && box->buffer[0] != '\0' && (SDL_GetTicks() / 500) % 2 == 0)
		snprintf(shown, sizeof(shown), "%s|", box->buffer);

	// подписи рисуются на холсте 2x
	Label label = { 0, 0, 0, 0, label_update, NULL, 2 };
	label.x = box->x + 3;
	label.y = box->y + (box->h - 6) / 2;
	label.text = shown;
	label_update(renderer, (Component*)&label);

	return true;
}

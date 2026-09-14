#include <Config.h>
#include <UTF8.h>
#include <ui/Components.h>
#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>

// notification text being typed in the news tab
char g_newsText[2048];
int  g_newsTextLen = 0;
bool g_newsTextFocus = false;

// right-side "recent notifications" section visibility (toggled by a button)
bool g_newsOpen = false;

// transient status line ("sent!", errors), drawn centered under the send button
int  g_newsStatusTime = 0;
char g_newsStatus[64] = "";

// geometry of the two boxes (design pixels)
#define NEWS_SEC_X	304	// recent notifications (right)
#define NEWS_SEC_Y	44
#define NEWS_SEC_W	172
#define NEWS_SEC_BOT	224

#define NEWS_CMP_X	4	// composed text preview (left)
#define NEWS_CMP_Y	44
#define NEWS_CMP_W	172
#define NEWS_CMP_BOT	224

static void news_status(const char* text)
{
	snprintf(g_newsStatus, sizeof(g_newsStatus), "%s", text);
	g_newsStatusTime = 180;
}

void news_set_status(const char* text)
{
	news_status(text);
}

void news_text_input(const char* text)
{
	if (!g_newsTextFocus)
		return;

	// ctrl+v is handled as a keydown paste; typing with ctrl held would
	// otherwise insert the shortcut's literal character
	if (SDL_GetModState() & KMOD_CTRL)
		return;

	for (const char* c = text; *c && g_newsTextLen < (int)sizeof(g_newsText) - 1; c++)
	{
		if ((unsigned char)*c >= 0x20 && (unsigned char)*c != 0x7F)
			g_newsText[g_newsTextLen++] = *c;
	}

	g_newsText[g_newsTextLen] = '\0';
}

static void news_text_paste(void)
{
	char* clip = SDL_GetClipboardText();
	if (!clip)
		return;

	for (char* c = clip; *c && g_newsTextLen < (int)sizeof(g_newsText) - 1; c++)
	{
		if ((unsigned char)*c >= 0x20 && (unsigned char)*c != 0x7F)
			g_newsText[g_newsTextLen++] = *c;
	}

	g_newsText[g_newsTextLen] = '\0';
	SDL_free(clip);
}

void news_text_key(SDL_Keycode key)
{
	if (!g_newsTextFocus)
		return;

	if (key == SDLK_BACKSPACE && g_newsTextLen > 0)
	{
		// back over whole utf8 codepoints, not bytes
		g_newsTextLen--;
		while (g_newsTextLen > 0 && ((unsigned char)g_newsText[g_newsTextLen] & 0xC0) == 0x80)
			g_newsTextLen--;

		g_newsText[g_newsTextLen] = '\0';
	}
	else if (key == SDLK_v && (SDL_GetModState() & KMOD_CTRL))
		news_text_paste();
	else if (key == SDLK_ESCAPE)
		g_newsTextFocus = false;
}

// ---- label font metrics (kept in sync with label_update in ui/Label.c) ----

static int news_char_width(utf8_char c)
{
	// label_update draws lowercased glyphs - fold before measuring
	if (c == 0x0401)
		c = 0x0451;
	else if (c >= 0x0410 && c <= 0x042F)
		c += 0x20;
	else if (c >= 'A' && c <= 'Z')
		c += 0x20;

	if (c == ' ')
		return 5;

	// escaped link characters (see news_markup_urls) always draw and take a cell
	if (c >= 0xE000 && c <= 0xE009)
		return 6;

	// color prefixes draw nothing
	if (c == '\\' || c == '@' || c == '&' || c == '/' || c == '|' ||
		c == '`' || c == '~' || c == 0x2116 || c == '<' || c == '>')
		return 0;

	int w = 6;
	switch (c)
	{
	case 'w': case 'm': case 'x': case 'n':
	case 0x043C: case 0x0434: case 0x0438: case 0x0439:
	case 0x044E: case 0x044C: case 0x043B: case 0x0448:
	case 0x0449: case 0x0446: case 0x0436:
		w += 2;
		break;
	}

	return w;
}

static int news_text_width(const char* text)
{
	int w = 0;
	size_t len = utf8_strlen(text);

	for (int i = 0; i < (int)len; i++)
		w += news_char_width(utf8_get(text, i));

	return w;
}

// encodes one codepoint into dst at pos; returns the new position
static size_t utf8_put(char* dst, size_t cap, size_t pos, utf8_char c)
{
	char buf[4];
	size_t n = 0;

	if (c < 0x80)
		buf[n++] = (char)c;
	else if (c < 0x800)
	{
		buf[n++] = (char)(0xC0 | (c >> 6));
		buf[n++] = (char)(0x80 | (c & 0x3F));
	}
	else if (c < 0x10000)
	{
		buf[n++] = (char)(0xE0 | (c >> 12));
		buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
		buf[n++] = (char)(0x80 | (c & 0x3F));
	}
	else
	{
		buf[n++] = (char)(0xF0 | (c >> 18));
		buf[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
		buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
		buf[n++] = (char)(0x80 | (c & 0x3F));
	}

	if (pos + n >= cap)
		return pos;

	for (size_t i = 0; i < n; i++)
		dst[pos + i] = buf[i];

	return pos + n;
}

// word-wraps a line into at most max_lines rows of max_px width
static int news_wrap_entry(const char* text, int max_px, char lines[][128], int max_lines)
{
	int count = 0;
	char line[128];
	char word[512];
	size_t line_len = 0, word_len = 0;
	int line_w = 0, word_w = 0;

	line[0] = '\0';
	word[0] = '\0';

	size_t len = utf8_strlen(text);
	for (int i = 0; i <= (int)len; i++)
	{
		bool last = (i == (int)len);
		utf8_char c = last ? ' ' : utf8_get(text, i);

		if (c == ' ' || c == '\n')
		{
			int sep = (line_len > 0) ? 5 : 0;

			if (line_len > 0 && line_w + sep + word_w > max_px && count < max_lines)
			{
				snprintf(lines[count], 128, "%s", line);
				count++;
				line_len = 0;
				line_w = 0;
				sep = 0;
			}

			if (word_len > 0 && count < max_lines)
			{
				if (line_len + sep + word_len < sizeof(line))
				{
					if (sep)
						line[line_len++] = ' ';

					for (size_t k = 0; k < word_len; k++)
						line[line_len + k] = word[k];

					line_len += word_len;
					line[line_len] = '\0';
					line_w += sep + word_w;
				}

				word_len = 0;
				word_w = 0;
			}

			if (c == '\n' && count < max_lines)
			{
				snprintf(lines[count], 128, "%s", line);
				count++;
				line_len = 0;
				line_w = 0;
				line[0] = '\0';
			}
		}
		else
		{
			word_len = utf8_put(word, sizeof(word), word_len, c);
			word[word_len] = '\0';
			word_w += news_char_width(c);
		}
	}

	if (line_len > 0 && count < max_lines)
	{
		snprintf(lines[count], 128, "%s", line);
		count++;
	}

	return count;
}

static void news_draw_box(SDL_Renderer* renderer, int x, int y, int w, int h)
{
	SDL_Rect dst = { x * INTERFACE_SCALE, y * INTERFACE_SCALE, w * INTERFACE_SCALE, h * INTERFACE_SCALE };
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderFillRect(renderer, &dst);
	SDL_SetRenderDrawColor(renderer, 105, 105, 105, 255);
	SDL_RenderDrawRect(renderer, &dst);
}

// marks up http/https links: the link is drawn blue and its color-trigger
// characters (/ & ~ \ | ` № < >) are replaced with private codepoints that
// label_update renders literally instead of switching color
static void news_markup_urls(char* buf, size_t cap)
{
	char tmp[4200];
	snprintf(tmp, sizeof(tmp), "%s", buf);

	size_t o = 0;
	for (size_t i = 0; tmp[i] != '\0' && o + 8 < cap; )
	{
		bool url = (strncmp(&tmp[i], "https://", 8) == 0 || strncmp(&tmp[i], "http://", 7) == 0);
		bool bounded = (i == 0 || tmp[i - 1] == ' ' || tmp[i - 1] == '\n');

		if (url && bounded)
		{
			buf[o++] = '/'; // blue link color

			for (; tmp[i] != '\0' && o + 8 < cap; i++)
			{
				char c = tmp[i];
				if (c == ' ' || c == '\n' || c == '\t')
					break;

				if (strncmp(&tmp[i], "https", 5) == 0 || strncmp(&tmp[i], "http", 4) == 0)
				{
					// part of the scheme or a nested "http" - copy as is
					buf[o++] = c;
					continue;
				}

				switch (c)
				{
				case '/': buf[o++] = (char)0xEE; buf[o++] = (char)0x80; buf[o++] = (char)0x80; break; // 0xE000
				case '&': buf[o++] = (char)0xEE; buf[o++] = (char)0x80; buf[o++] = (char)0x81; break; // 0xE001
				case '~': buf[o++] = (char)0xEE; buf[o++] = (char)0x80; buf[o++] = (char)0x82; break; // 0xE002
				case '\\': buf[o++] = (char)0xEE; buf[o++] = (char)0x80; buf[o++] = (char)0x83; break; // 0xE003
				case '|': buf[o++] = (char)0xEE; buf[o++] = (char)0x80; buf[o++] = (char)0x84; break; // 0xE004
				case '@': buf[o++] = (char)0xEE; buf[o++] = (char)0x80; buf[o++] = (char)0x85; break; // 0xE005
				case '`': buf[o++] = (char)0xEE; buf[o++] = (char)0x80; buf[o++] = (char)0x86; break; // 0xE006
				case '<': buf[o++] = (char)0xEE; buf[o++] = (char)0x80; buf[o++] = (char)0x88; break; // 0xE008
				case '>': buf[o++] = (char)0xEE; buf[o++] = (char)0x80; buf[o++] = (char)0x89; break; // 0xE009
				default:
					buf[o++] = c;
					break;
				}
			}

			buf[o++] = '~'; // back to the base color
			continue;
		}

		buf[o++] = tmp[i++];
	}

	buf[o] = '\0';
}

// drops trailing line breaks and spaces (a trailing "\n" would render as an empty row)
static void news_trim_tail(char* s)
{
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t'))
		s[--len] = '\0';
}

bool newstext_update(SDL_Renderer* renderer, struct _Component* component)
{
	NewsTextInput* box = (NewsTextInput*)component;
	SDL_Rect dst = { box->x * INTERFACE_SCALE, box->y * INTERFACE_SCALE, box->w * INTERFACE_SCALE, box->h * INTERFACE_SCALE };

	int mouse_x, mouse_y;
	float scale_x, scale_y;
	Uint32 flags = SDL_GetMouseState(&mouse_x, &mouse_y);
	SDL_RenderGetScale(renderer, &scale_x, &scale_y);

	mouse_x /= scale_x;
	mouse_y /= scale_y;

	if (flags & SDL_BUTTON(1))
		g_newsTextFocus = (mouse_x >= dst.x && mouse_y >= dst.y && mouse_x < dst.x + dst.w && mouse_y < dst.y + dst.h);

	// box: dark interior, grey border (white while focused)
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderFillRect(renderer, &dst);

	if (g_newsTextFocus)
		SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
	else
		SDL_SetRenderDrawColor(renderer, 105, 105, 105, 255);

	SDL_RenderDrawRect(renderer, &dst);

	// the "\n" pair is shown as a white "n" in the input line (a lone slash
	// still turns the text red - only "\n" is exempt)
	char shown[4096];
	size_t o = 0;

	for (int i = 0; g_newsText[i] != '\0' && o < sizeof(shown) - 4; i++)
	{
		if (g_newsText[i] == '\\' && g_newsText[i + 1] == 'n')
		{
			shown[o++] = '\\';
			shown[o++] = '~';
			shown[o++] = 'n';
			i++;
		}
		else
			shown[o++] = g_newsText[i];
	}
	shown[o] = '\0';

	if (g_newsText[0] == '\0')
		snprintf(shown, sizeof(shown), "%s", g_newsTextFocus ? "|" : "|enter notification text");
	else if (g_newsTextFocus && (SDL_GetTicks() / 500) % 2 == 0)
		snprintf(shown, sizeof(shown), "%s|", shown);

	// labels are drawn on the 2x canvas: scale 2 keeps design coordinates
	Label label = { 0, 0, 0, 0, label_update, NULL, 2 };
	label.x = box->x + 4;
	label.y = box->y + (box->h - 6) / 2;
	label.text = shown;
	label_update(renderer, (Component*)&label);

	// ---- left box: preview of the text being composed ----
	news_draw_box(renderer, NEWS_CMP_X, NEWS_CMP_Y, NEWS_CMP_W, NEWS_CMP_BOT - NEWS_CMP_Y);

	char disp[4096];
	o = 0;

	for (int i = 0; g_newsText[i] != '\0' && o < sizeof(disp) - 4; i++)
	{
		// "\n" becomes a real line break here - visual only
		if (g_newsText[i] == '\\' && g_newsText[i + 1] == 'n')
		{
			disp[o++] = '\n';
			i++;
		}
		else
			disp[o++] = g_newsText[i];
	}
	disp[o] = '\0';

	if (disp[0] == '\0')
		snprintf(disp, sizeof(disp), "%s", "|nothing to send yet");

	if (g_newsTextFocus && g_newsText[0] != '\0' && (SDL_GetTicks() / 500) % 2 == 0)
		snprintf(disp, sizeof(disp), "%s|", disp);

	// links render blue with literal slashes, same as in the recent list
	news_markup_urls(disp, sizeof(disp));

	static int s_cmpScroll = 0;

	char lines[64][128];
	int n = news_wrap_entry(disp, NEWS_CMP_W - 8, lines, 64);

	// scroll the preview with the wheel while the cursor is over the box
	{
		int mx, my;
		float sx, sy;
		SDL_GetMouseState(&mx, &my);
		SDL_RenderGetScale(renderer, &sx, &sy);
		mx = (int)(mx / sx / INTERFACE_SCALE);
		my = (int)(my / sy / INTERFACE_SCALE);

		bool over = (mx >= NEWS_CMP_X && mx < NEWS_CMP_X + NEWS_CMP_W &&
			my >= NEWS_CMP_Y && my < NEWS_CMP_BOT);

		if (over)
			s_cmpScroll -= g_mouseWheel;

		int maxVis = (NEWS_CMP_BOT - 2 - (NEWS_CMP_Y + 12)) / 8;
		int maxScroll = n - maxVis;
		if (maxScroll < 0)
			maxScroll = 0;
		if (s_cmpScroll > maxScroll)
			s_cmpScroll = maxScroll;
		if (s_cmpScroll < 0)
			s_cmpScroll = 0;
	}

	for (int l = 0; l + s_cmpScroll < n && NEWS_CMP_Y + 12 + l * 8 + 8 <= NEWS_CMP_BOT - 2; l++)
	{
		label.x = NEWS_CMP_X + 4;
		label.y = NEWS_CMP_Y + 8 + l * 8;
		label.text = lines[l + s_cmpScroll];
		label_update(renderer, (Component*)&label);
	}

	return true;
}

bool newsstatus_update(SDL_Renderer* renderer, struct _Component* component)
{
	(void)component;

	if (g_newsStatus[0] == '\0')
		return true;

	// centered under the send button
	int w = news_text_width(g_newsStatus);
	Label label = { 240 - w / 2, 68, 0, 0, label_update, g_newsStatus, 2 };
	label_update(renderer, (Component*)&label);

	return true;
}

bool newslist_update(SDL_Renderer* renderer, struct _Component* component)
{
	NewsList* list = (NewsList*)component;
	static int s_page = 0;
	static int s_entryScroll = 0;
	static bool s_mouseHeld = false;

	// expire the transient status label
	if (g_newsStatusTime > 0 && --g_newsStatusTime == 0)
		g_newsStatus[0] = '\0';

	if (!g_newsOpen)
		return true;

	news_draw_box(renderer, NEWS_SEC_X, NEWS_SEC_Y, NEWS_SEC_W, NEWS_SEC_BOT - NEWS_SEC_Y);

	Label label = { 0, 0, 0, 0, label_update, NULL, 2 };
	label.x = NEWS_SEC_X + 4;
	label.y = NEWS_SEC_Y + 4;
	label.text = "|recent:";
	label_update(renderer, (Component*)&label);

	size_t count = news_count();

	if (count == 0)
	{
		s_page = 0;
		s_entryScroll = 0;

		label.x = NEWS_SEC_X + 4;
		label.y = NEWS_SEC_Y + 14;
		label.text = "|empty";
		label_update(renderer, (Component*)&label);
		return true;
	}

	// page switching: prev/next + delete at the bottom of the box
	int mouse_x, mouse_y;
	float scale_x, scale_y;
	Uint32 flags = SDL_GetMouseState(&mouse_x, &mouse_y);
	SDL_RenderGetScale(renderer, &scale_x, &scale_y);
	mouse_x = (int)(mouse_x / scale_x / INTERFACE_SCALE);
	mouse_y = (int)(mouse_y / scale_y / INTERFACE_SCALE);

	bool down = (flags & SDL_BUTTON(1)) != 0;
	bool clicked = down && !s_mouseHeld;
	s_mouseHeld = down;

	int prevW = news_text_width("prev");
	int nextW = news_text_width("next");
	int delW = news_text_width("delete");

	int btnY = NEWS_SEC_BOT - 14;
	int prevX = NEWS_SEC_X + 4;
	int nextX = NEWS_SEC_X + NEWS_SEC_W - 4 - nextW;
	int delX = NEWS_SEC_X + (NEWS_SEC_W - delW) / 2;

	bool hovPrev = (mouse_x >= prevX - 2 && mouse_x < prevX + prevW + 2 && mouse_y >= btnY - 2 && mouse_y < btnY + 9);
	bool hovNext = (mouse_x >= nextX - 2 && mouse_x < nextX + nextW + 2 && mouse_y >= btnY - 2 && mouse_y < btnY + 9);
	bool hovDel = (mouse_x >= delX - 2 && mouse_x < delX + delW + 2 && mouse_y >= btnY - 2 && mouse_y < btnY + 9);

	char prevLbl[64];
	snprintf(prevLbl, sizeof(prevLbl), "%s", (s_page > 0 || hovPrev) ? "prev" : "|prev");

	char nextLbl[64];
	snprintf(nextLbl, sizeof(nextLbl), "%s", (s_page < (int)count - 1 || hovNext) ? "next" : "|next");

	// delete: red normally, white on hover
	char delLbl[64];
	snprintf(delLbl, sizeof(delLbl), "%s", hovDel ? "~delete" : "\\delete");

	label.x = prevX;
	label.y = btnY;
	label.text = prevLbl;
	label_update(renderer, (Component*)&label);

	label.x = nextX;
	label.y = btnY;
	label.text = nextLbl;
	label_update(renderer, (Component*)&label);

	label.x = delX;
	label.y = btnY;
	label.text = delLbl;
	label_update(renderer, (Component*)&label);

	if (clicked)
	{
		if (hovPrev && s_page > 0)
		{
			s_page--;
			s_entryScroll = 0;
		}

		if (hovNext && s_page < (int)count - 1)
		{
			s_page++;
			s_entryScroll = 0;
		}

		if (hovDel)
		{
			uint32_t delId;
			char delText[2048];
			if (news_get(s_page, &delId, delText, sizeof(delText)) && news_delete(delId))
			{
				size_t left = news_count();
				if (s_page >= (int)left)
					s_page = (int)left - 1;
				if (s_page < 0)
					s_page = 0;
				s_entryScroll = 0;
			}
		}
	}

	if (s_page >= (int)count)
		s_page = (int)count - 1;
	if (s_page < 0)
		s_page = 0;

	// page counter in the header row
	char counter[32];
	snprintf(counter, sizeof(counter), "|%d/%d", s_page + 1, (int)count);
	label.x = NEWS_SEC_X + NEWS_SEC_W - 4 - news_text_width(counter);
	label.y = NEWS_SEC_Y + 4;
	label.text = counter;
	label_update(renderer, (Component*)&label);

	// the notification itself
	uint32_t id;
	char text[2048];
	if (!news_get(s_page, &id, text, sizeof(text)))
		return true;

	news_trim_tail(text);

	char entry[2100];
	snprintf(entry, sizeof(entry), "|%u %s", id, text);
	news_markup_urls(entry, sizeof(entry));

	char lines[64][128];
	int n = news_wrap_entry(entry, NEWS_SEC_W - 8, lines, 64);

	// scroll long entries with the wheel while the cursor is over the box
	{
		bool over = (mouse_x >= NEWS_SEC_X && mouse_x < NEWS_SEC_X + NEWS_SEC_W &&
			mouse_y >= NEWS_SEC_Y && mouse_y < NEWS_SEC_BOT);

		if (over)
			s_entryScroll -= g_mouseWheel;

		int maxVis = (btnY - 2 - (NEWS_SEC_Y + 14)) / 8;
		int maxScroll = n - maxVis;
		if (maxScroll < 0)
			maxScroll = 0;
		if (s_entryScroll > maxScroll)
			s_entryScroll = maxScroll;
		if (s_entryScroll < 0)
			s_entryScroll = 0;
	}

	for (int l = 0; l + s_entryScroll < n && NEWS_SEC_Y + 14 + l * 8 + 8 <= btnY - 2; l++)
	{
		label.x = NEWS_SEC_X + 4;
		label.y = NEWS_SEC_Y + 14 + l * 8;
		label.text = lines[l + s_entryScroll];
		label_update(renderer, (Component*)&label);
	}

	(void)list;
	return true;
}


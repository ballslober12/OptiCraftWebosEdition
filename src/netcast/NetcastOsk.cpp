#ifdef NETCAST_PLATFORM
#include "netcast/NetcastOsk.h"

#include "Gui.h"
#include "FontRenderer.h"
#include "pc/lwjgl/Keyboard.h"

namespace netcast
{
namespace
{
const char *ROWS[] = {
	"1234567890-=",
	"qwertyuiop[]",
	"asdfghjkl;'/",
	"zxcvbnm,.?!:",
};
const int COLS = 12, ROWN = 4;

struct OskGui : Gui
{
	using Gui::drawRect;
};

bool g_open = false;
bool g_field = false;
bool g_autoOpen = false;
bool g_shift = false;
int  g_selX = 0, g_selY = 0;
std::string g_text;
}

bool oskActive() { return g_open && g_field; }
bool oskCanOpen() { return g_field; }
void oskOpen() { if (g_field) { g_open = true; g_selX = g_selY = 0; } }
void oskClose() { g_open = false; }
void oskAutoOpenNextField() { g_autoOpen = true; }

void oskInput(int dx, int dy, bool type, bool back, bool space, bool shift, bool enter, bool close)
{
	if (!oskActive())
		return;
	g_selX = (g_selX + dx + COLS) % COLS;
	g_selY = (g_selY + dy + ROWN) % ROWN;
	if (type)
	{
		char c = ROWS[g_selY][g_selX];
		if (g_shift && c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		lwjgl::Keyboard::detail::pushChar((int)(unsigned char)c);
	}
	if (back)
	{
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_BACK, true);
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_BACK, false);
	}
	if (space)
		lwjgl::Keyboard::detail::pushChar((int)' ');
	if (shift)
		g_shift = !g_shift;
	if (enter)
	{
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_RETURN, true);
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_RETURN, false);
		g_open = false;
	}
	if (close)
		g_open = false;
}

void oskRender(FontRenderer *font, int sw, int sh, const std::string *fieldText)
{
	const bool had = g_field;
	g_field = fieldText != nullptr;
	if (!g_field)
	{
		g_open = false;
		return;
	}
	g_text = *fieldText;
	if (!had && g_autoOpen)
	{
		g_autoOpen = false;
		g_open = true;
		g_selX = g_selY = 0;
	}
	if (!g_open || font == nullptr)
		return;

	static OskGui gui;
	const int keyW = 22, keyH = 18, gap = 3;
	const int panelW = COLS * keyW + gap * 2;
	const int panelH = ROWN * keyH + 16 + 24;
	const int px = (sw - panelW) / 2;
	const int py = sh - panelH - 4;

	gui.drawRect(px - 2, py - 2, px + panelW + 2, py + panelH + 2, 0xdd000000);
	gui.drawRect(px, py, px + panelW, py + panelH, 0xff202020);

	std::string preview = g_text + "_";
	while (preview.size() > 1 && font->getStringWidth(preview) > panelW - 10)
		preview.erase(0, 1);
	gui.drawRect(px + 3, py + 3, px + panelW - 3, py + 14, 0xff000000);
	gui.drawString(font, preview, px + 5, py + 5, g_shift ? 0xffff80 : 0xffffff);

	const int gridY = py + 16;
	for (int r = 0; r < ROWN; r++)
		for (int c = 0; c < COLS; c++)
		{
			const int x0 = px + gap + c * keyW, y0 = gridY + r * keyH;
			const bool sel = (r == g_selY && c == g_selX);
			gui.drawRect(x0, y0, x0 + keyW - 2, y0 + keyH - 2, sel ? 0xffffffff : 0xff707070);
			gui.drawRect(x0 + 1, y0 + 1, x0 + keyW - 3, y0 + keyH - 3, sel ? 0xff606060 : 0xff101010);
			char ch = ROWS[r][c];
			if (g_shift && ch >= 'a' && ch <= 'z')
				ch = (char)(ch - 'a' + 'A');
			const char s[2] = { ch, 0 };
			gui.drawString(font, std::string(s), x0 + 8, y0 + 5, 0xffffff);
		}
	gui.drawString(font, "A type  X back  Y space  LB shift  Start enter  B close", px + 2, py + panelH - 20, 0xcccccc);
}
}
#endif

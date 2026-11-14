// WebOSPlatform.cpp -- see WebOSPlatform.h.
//
// Bring-up: SDL2 video (the webosbrew SDL-webOS build: Wayland + xkbcommon),
// an OpenGL ES 1.x window from SDL_CreateWindow + SDL_GL_CreateContext, then the
// event loop. The GLES1 renderer (src/webos/render) talks to the same context
// directly, so nothing here creates an EGL surface by hand: SDL owns it.
//
// Input comes from three places and all of them are polled on the main thread:
//   * the Magic Remote / d-pad, delivered by SDL as key events;
//   * a USB mouse or the remote pointer, delivered as mouse events;
//   * gamepads, read through SDL_GameController each frame.
// Everything is turned into the lwjgl::Keyboard / lwjgl::Mouse queues the shared
// game code already consumes, plus the analog snapshot platformGamepadSnapshot()
// reads for smooth stick walking.
#ifdef WEBOS_PLATFORM

#include "webos/WebOSPlatform.h"
#include "webos/WebOSOsk.h"
#include "webos/WebOSRender.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include <GLES/gl.h>

#include "lwjgl/Keyboard.h"
#include "lwjgl/Mouse.h"

namespace webos
{

// Bumped whenever the webOS platform layer changes in a way that shows up in the
// log. __DATE__/__TIME__ below is the compile time of THIS file only, so it does
// not change when the renderer or a shared file is rebuilt -- the tag does.
#define WEBOS_BUILD_TAG "opt4 (native scale, view radius 2, CTM off)"

unsigned g_statDraws = 0;
double   g_statCopyMs = 0.0;
double   g_statAudioCpu = 0.0;
unsigned g_statUploads = 0;
double   g_statUploadKB = 0.0;
double   g_statUploadMs = 0.0;

namespace
{

// ---------------------------------------------------------------- config ----
std::map<std::string, int> g_cfg;
bool g_cfgLoaded = false;

std::string g_exeDir;
std::string resolveExeDir(const char *argv0)
{
	char buf[4096];
	ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	std::string path;
	if (n > 0)
	{
		buf[n] = 0;
		path = buf;
	}
	else if (argv0 != nullptr && argv0[0] != 0)
	{
		path = argv0;
	}
	const size_t slash = path.find_last_of('/');
	if (slash == std::string::npos)
	{
		if (::getcwd(buf, sizeof(buf)) != nullptr)
			return std::string(buf) + "/";
		return "./";
	}
	return path.substr(0, slash + 1);
}

void loadConfig()
{
	if (g_cfgLoaded)
		return;
	g_cfgLoaded = true;
	std::FILE *f = std::fopen((g_exeDir + "webos.cfg").c_str(), "r");
	if (f == nullptr)
		return;
	char line[256];
	while (std::fgets(line, sizeof(line), f) != nullptr)
	{
		char *eq = std::strchr(line, '=');
		if (eq == nullptr || line[0] == '#')
			continue;
		*eq = 0;
		std::string key = line;
		while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
			key.pop_back();
		g_cfg[key] = std::atoi(eq + 1);
	}
	std::fclose(f);
}

int g_logFd = -2;

// -------------------------------------------------------------- SDL state ----
SDL_Window *g_window = nullptr;
SDL_GLContext g_context = nullptr;
int g_width = 1280;
int g_height = 720;
bool g_sdlUp = false;
bool g_glUp = false;

std::atomic<bool> g_close(false);
std::atomic<bool> g_focus(true);

// --------------------------------------------------- main-thread state ----
// lwjgl::Keyboard::Key values (DirectInput scan codes).
enum
{
	LK_ESCAPE = 0x01, LK_1 = 0x02, LK_0 = 0x0B, LK_MINUS = 0x0C, LK_EQUALS = 0x0D,
	LK_TAB = 0x0F, LK_Q = 0x10, LK_W = 0x11, LK_E = 0x12, LK_T = 0x14, LK_P = 0x19,
	LK_LBRACKET = 0x1A, LK_RBRACKET = 0x1B, LK_LCONTROL = 0x1D,
	LK_A = 0x1E, LK_S = 0x1F, LK_D = 0x20, LK_L = 0x26,
	LK_SEMICOLON = 0x27, LK_APOSTROPHE = 0x28, LK_GRAVE = 0x29, LK_LSHIFT = 0x2A,
	LK_BACKSLASH = 0x2B, LK_Z = 0x2C, LK_M = 0x32, LK_COMMA = 0x33, LK_PERIOD = 0x34,
	LK_SLASH = 0x35, LK_RSHIFT = 0x36, LK_SPACE = 0x39,
	LK_NUMPADENTER = 0x9C, LK_RCONTROL = 0x9D, LK_DIVIDE = 0xB5, LK_RMENU = 0xB8,
	LK_HOME = 0xC7, LK_UP = 0xC8, LK_PRIOR = 0xC9, LK_LEFT = 0xCB, LK_RIGHT = 0xCD,
	LK_END = 0xCF, LK_DOWN = 0xD0, LK_NEXT = 0xD1, LK_INSERT = 0xD2, LK_DELETE = 0xD3,
};

bool g_vkDown[256] = {};
bool g_mouseBtn[3] = {};
int  g_cursorX = 640;
int  g_cursorY = 360;
bool g_logInput = false;

// remote arrows (held)
bool g_remoteHeld[4] = {}; // up, down, left, right

// gamepad
struct Pad
{
	SDL_GameController *ctrl = nullptr;
	SDL_JoystickID id = -1;
	std::string name;
	float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
	bool connected = false;
};
Pad g_pad;
bool g_btnDown[SDL_CONTROLLER_BUTTON_MAX] = {};
int  g_hatX = 0;
int  g_hatY = 0;
int  g_prevHatX = 0;
int  g_prevHatY = 0;

void pushKey(int key, bool down)
{
	if (key <= 0 || key >= 256)
		return;
	lwjgl::Keyboard::detail::pushKey(key, down);
}

// Set a virtual key; emits an event only on change so several producers
// (keyboard, remote, pad) can drive the same key without double events.
void setVKey(int key, bool down)
{
	if (key <= 0 || key >= 256 || g_vkDown[key] == down)
		return;
	g_vkDown[key] = down;
	pushKey(key, down);
}

void clampCursor()
{
	g_cursorX = std::max(0, std::min(g_width - 1, g_cursorX));
	g_cursorY = std::max(0, std::min(g_height - 1, g_cursorY));
}

void moveCursor(int dx, int dy)
{
	if (dx == 0 && dy == 0)
		return;
	g_cursorX += dx;
	g_cursorY += dy;
	clampCursor();
	lwjgl::Mouse::detail::pushMotion(g_cursorX, g_cursorY, dx, dy);
}

void setMouseButton(int button, bool down)
{
	if (button < 0 || button > 2 || g_mouseBtn[button] == down)
		return;
	g_mouseBtn[button] = down;
	lwjgl::Mouse::detail::pushButton(button, down, g_cursorX, g_cursorY);
}

// ------------------------------------------------- SDL key -> LWJGL key ----
// Layout-independent mapping: the same table the McBetaCpp webOS port uses, so
// a USB keyboard behaves exactly like it does on the other ports.
int keySDLToLWJGL(SDL_Keycode key)
{
	switch (key)
	{
		case SDLK_SLEEP: return 223;
		case SDLK_POWER: return 222;
		case SDLK_APPLICATION: return 221;
		case SDLK_CAPSLOCK: return 58;
		case SDLK_KP_MULTIPLY: return 55;
		case SDLK_DELETE: return 211;
		case SDLK_COMMA: return 51;
		case SDLK_r: return 19;
		case SDLK_t: return 20;
		case SDLK_BACKQUOTE: return 41;
		case SDLK_END: return 207;
		case SDLK_k: return 37;
		case SDLK_m: return 50;
		case SDLK_x: return 45;
		case SDLK_DOWN: return 208;
		case SDLK_g: return 34;
		case SDLK_f: return 33;
		case SDLK_c: return 46;
		case SDLK_a: return 30;
		case SDLK_SCROLLLOCK: return 70;
		case SDLK_QUOTE: return 40;
		case SDLK_KP_EQUALS: return 141;
		case SDLK_z: return 44;
		case SDLK_BACKSPACE: return 14;
		case SDLK_6: return 7;
		case SDLK_h: return 35;
		case SDLK_5: return 6;
		case SDLK_v: return 47;
		case SDLK_BACKSLASH: return 43;
		case SDLK_8: return 9;
		case SDLK_EQUALS: return 13;
		case SDLK_LALT: return 56;
		case SDLK_F12: return 88;
		case SDLK_RSHIFT: return 54;
		case SDLK_ESCAPE: return 1;
		case SDLK_LGUI: return 219;
		case SDLK_LSHIFT: return 42;
		case SDLK_F9: return 67;
		case SDLK_LCTRL: return 29;
		case SDLK_F10: return 68;
		case SDLK_4: return 5;
		case SDLK_e: return 18;
		case SDLK_SPACE: return 57;
		case SDLK_2: return 3;
		case SDLK_F11: return 87;
		case SDLK_o: return 24;
		case SDLK_KP_5: return 76;
		case SDLK_RGUI: return 220;
		case SDLK_F13: return 100;
		case SDLK_KP_DIVIDE: return 181;
		case SDLK_KP_0: return 82;
		case SDLK_9: return 10;
		case SDLK_q: return 16;
		case SDLK_KP_3: return 81;
		case SDLK_STOP: return 149;
		case SDLK_b: return 48;
		case SDLK_RIGHT: return 205;
		case SDLK_SEMICOLON: return 39;
		case SDLK_MINUS: return 12;
		case SDLK_l: return 38;
		case SDLK_RIGHTBRACKET: return 27;
		case SDLK_w: return 17;
		case SDLK_TAB: return 15;
		case SDLK_y: return 21;
		case SDLK_NUMLOCKCLEAR: return 69;
		case SDLK_KP_MINUS: return 74;
		case SDLK_u: return 22;
		case SDLK_F17: return 104;
		case SDLK_p: return 25;
		case SDLK_F4: return 62;
		case SDLK_1: return 2;
		case SDLK_0: return 11;
		case SDLK_LEFTBRACKET: return 26;
		case SDLK_CLEAR: return 218;
		case SDLK_F1: return 59;
		case SDLK_F2: return 60;
		case SDLK_F3: return 61;
		case SDLK_F5: return 63;
		case SDLK_F6: return 64;
		case SDLK_F7: return 65;
		case SDLK_i: return 23;
		case SDLK_F16: return 103;
		case SDLK_F8: return 66;
		case SDLK_d: return 32;
		case SDLK_KP_7: return 71;
		case SDLK_KP_8: return 72;
		case SDLK_KP_9: return 73;
		case SDLK_7: return 8;
		case SDLK_KP_4: return 75;
		case SDLK_KP_6: return 77;
		case SDLK_j: return 36;
		case SDLK_HOME: return 199;
		case SDLK_n: return 49;
		case SDLK_KP_1: return 79;
		case SDLK_KP_ENTER: return 156;
		case SDLK_KP_2: return 80;
		case SDLK_SLASH: return 53;
		case SDLK_INSERT: return 210;
		case SDLK_RALT: return 184;
		case SDLK_s: return 31;
		case SDLK_KP_DECIMAL: return 83;
		case SDLK_F14: return 101;
		case SDLK_F15: return 102;
		case SDLK_F18: return 105;
		case SDLK_F19: return 113;
		case SDLK_3: return 4;
		case SDLK_RETURN: return 28;
		case SDLK_PERIOD: return 52;
		case SDLK_AT: return 145;
		case SDLK_COLON: return 146;
		case SDLK_RCTRL: return 157;
		case SDLK_KP_PLUS: return 78;
		case SDLK_KP_COMMA: return 179;
		case SDLK_SYSREQ: return 183;
		case SDLK_PAUSE: return 197;
		case SDLK_PRIOR: return 201;
		case SDLK_UP: return 200;
		case SDLK_LEFT: return 203;
		case SDLK_AC_FORWARD: return 209;
		default: return 0;
	}
}

// `code` is the layout-specific key; the remote reports the same few keys with
// both fields, but a USB keyboard may differ (e.g. AZERTY `a`), so prefer the
// character when one is present.
int keyForEvent(const SDL_KeyboardEvent &key)
{
	if (key.keysym.scancode == SDL_SCANCODE_AC_BACK)
		return LK_ESCAPE;
	return keySDLToLWJGL(key.keysym.sym);
}

// ------------------------------------------------------------- gamepad ----
void openPad(int deviceIndex)
{
	if (g_pad.ctrl != nullptr)
		return;
	SDL_GameController *ctrl = SDL_GameControllerOpen(deviceIndex);
	if (ctrl == nullptr)
	{
		// SDL has no mapping for this pad (common for cheap TV remotes with a
		// gamepad mode). Treat it as raw joystick only if it is one.
		log("controller %d: SDL_GameControllerOpen failed: %s", deviceIndex, SDL_GetError());
		return;
	}
	g_pad.ctrl = ctrl;
	g_pad.id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ctrl));
	g_pad.name = SDL_GameControllerName(ctrl) != nullptr ? SDL_GameControllerName(ctrl) : "?";
	g_pad.connected = true;
	log("controller connected: %s (instance %d)", g_pad.name.c_str(), static_cast<int>(g_pad.id));
}

void closePad(SDL_JoystickID id)
{
	if (g_pad.ctrl == nullptr || g_pad.id != id)
		return;
	log("controller disconnected: %s", g_pad.name.c_str());
	SDL_GameControllerClose(g_pad.ctrl);
	g_pad = Pad();
	std::memset(g_btnDown, 0, sizeof(g_btnDown));
	g_hatX = g_hatY = 0;
}

// Radial deadzone with rescale so the stick still reaches +-1.
void applyDeadzone(float &x, float &y, float dead)
{
	const float m = std::sqrt(x * x + y * y);
	if (m <= dead || m <= 0.0f)
	{
		x = y = 0.0f;
		return;
	}
	const float scaled = std::min(1.0f, (m - dead) / (1.0f - dead));
	x = x / m * scaled;
	y = y / m * scaled;
}

float axisValue(SDL_GameControllerAxis axis)
{
	if (g_pad.ctrl == nullptr)
		return 0.0f;
	const Sint16 raw = SDL_GameControllerGetAxis(g_pad.ctrl, axis);
	return raw < 0 ? std::max(-1.0f, raw / 32768.0f) : std::min(1.0f, raw / 32767.0f);
}

bool padButton(SDL_GameControllerButton button)
{
	if (g_pad.ctrl == nullptr || button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX)
		return false;
	return g_btnDown[button] || SDL_GameControllerGetButton(g_pad.ctrl, button) != 0;
}

// --------------------------------------------------------------- helpers ----
double cpuSeconds(clockid_t id)
{
	struct timespec ts;
	clock_gettime(id, &ts);
	return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void logSystemInfo()
{
	log("cpus online: %ld", sysconf(_SC_NPROCESSORS_ONLN));
	if (std::FILE *f = std::fopen("/proc/cpuinfo", "r"))
	{
		char line[256];
		int shown = 0;
		while (std::fgets(line, sizeof(line), f) && shown < 6)
		{
			if (std::strstr(line, "model name") || std::strstr(line, "BogoMIPS") ||
			    std::strstr(line, "Features") || std::strstr(line, "CPU part") ||
			    std::strstr(line, "Hardware"))
			{
				line[std::strcspn(line, "\n")] = 0;
				log("cpuinfo: %s", line);
				++shown;
			}
		}
		std::fclose(f);
	}
	if (std::FILE *f = std::fopen("/proc/meminfo", "r"))
	{
		char line[256];
		while (std::fgets(line, sizeof(line), f))
		{
			if (std::strstr(line, "MemTotal") || std::strstr(line, "MemFree"))
			{
				line[std::strcspn(line, "\n")] = 0;
				log("meminfo: %s", line);
			}
		}
		std::fclose(f);
	}
}

} // namespace

// ----------------------------------------------------------- public API ----
void log(const char *fmt, ...)
{
	char buf[1024];
	int n = std::snprintf(buf, sizeof(buf), "[webos] ");
	va_list ap;
	va_start(ap, fmt);
	n += std::vsnprintf(buf + n, sizeof(buf) - static_cast<size_t>(n) - 2, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > static_cast<int>(sizeof(buf)) - 2)
		n = static_cast<int>(sizeof(buf)) - 2;
	if (buf[n - 1] != '\n')
		buf[n++] = '\n';
	// write(2): the webOS app framework redirects stdio, not the file descriptor.
	(void)::write(2, buf, static_cast<size_t>(n));
	if (g_logFd == -2)
	{
		// The app bundle is read-only on a TV, so fall back to $HOME and then
		// /tmp; the first writable location wins for the whole session.
		static const char *suffix = "webos.log";
		const char *home = std::getenv("HOME");
		const std::string candidates[] = {
			exeDir() + suffix,
			home != nullptr && home[0] != 0 ? std::string(home) + "/" + suffix : std::string(),
			std::string("/tmp/") + suffix,
		};
		g_logFd = -1;
		for (const std::string &path : candidates)
		{
			if (path.empty())
				continue;
			const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (fd >= 0)
			{
				g_logFd = fd;
				break;
			}
		}
	}
	if (g_logFd >= 0)
		(void)::write(g_logFd, buf, static_cast<size_t>(n));
}

int configInt(const char *key, int fallback)
{
	loadConfig();
	auto it = g_cfg.find(key);
	return it == g_cfg.end() ? fallback : it->second;
}

const std::string &exeDir()
{
	if (g_exeDir.empty())
		g_exeDir = resolveExeDir(nullptr);
	return g_exeDir;
}

int  width()  { return g_width; }
int  height() { return g_height; }
bool closeRequested() { return g_close.load(); }
void requestClose()   { g_close = true; }
bool hasFocus()       { return g_focus.load(); }

bool padSnapshot(float &lx, float &ly, float &rx, float &ry)
{
	lx = g_pad.lx;
	ly = g_pad.ly;
	rx = g_pad.rx;
	ry = g_pad.ry;
	return g_pad.connected;
}

bool initialize(int argc, char **argv)
{
	g_exeDir = resolveExeDir(argc > 0 && argv != nullptr ? argv[0] : nullptr);
	loadConfig();
	g_logInput = configInt("log_input", 0) != 0;

	log("OptiCraft webOS, exe dir %s", g_exeDir.c_str());
	for (int i = 0; i < argc; ++i)
		log("argv[%d] = %s", i, argv[i] != nullptr ? argv[i] : "(null)");
	log("build %s %s", __DATE__, __TIME__);
	log("build tag: %s", WEBOS_BUILD_TAG);
	log("settings: size=%dx%d fullscreen=%d swap_interval=%d opaque=%d fb16=%d use_vbo=%d copy_fb=%d msaa=%d",
	    configInt("width", 1280), configInt("height", 720), configInt("fullscreen", 0),
	    configInt("swap_interval", 0), configInt("opaque", 1), configInt("fb16", 0),
	    configInt("use_vbo", 0), configInt("copy_fb", 0), configInt("msaa", 0));
	logSystemInfo();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0)
	{
		log("SDL_Init failed: %s", SDL_GetError());
		return false;
	}
	g_sdlUp = true;

	SDL_version ver;
	SDL_GetVersion(&ver);
	log("SDL %d.%d.%d (%s), video driver %s", ver.major, ver.minor, ver.patch,
	    SDL_GetRevision(), SDL_GetCurrentVideoDriver() != nullptr ? SDL_GetCurrentVideoDriver() : "?");

	// The webOS compositor presents the window full screen whatever size we ask
	// for; the drawable size is the panel resolution. width/height in webos.cfg
	// only shrink the render target for weak GPUs.
	int wantW = configInt("width", 1920);
	int wantH = configInt("height", 1080);
	SDL_DisplayMode desktop;
	if (SDL_GetDesktopDisplayMode(0, &desktop) == 0 && desktop.w > 0 && desktop.h > 0)
	{
		if (configInt("custom_size", 0) == 0)
		{
			wantW = desktop.w;
			wantH = desktop.h;
		}
	}
	wantW = std::max(320, wantW);
	wantH = std::max(240, wantH);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	// Opaque window (the game clears with alpha 0). alpha=1 in webos.cfg restores
	// an 8-bit destination alpha channel, which only makes sense on a compositor
	// that expects it.
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, configInt("alpha", 0) != 0 ? 8 : 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL;
	if (configInt("fullscreen", 0) != 0)
		windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	const int depthSizes[] = {configInt("depth", 24), 16};
	for (int depth : depthSizes)
	{
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth);
		g_window = SDL_CreateWindow("OptiCraft", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		                            wantW, wantH, windowFlags);
		if (g_window == nullptr)
		{
			log("SDL_CreateWindow (depth %d) failed: %s", depth, SDL_GetError());
			continue;
		}
		g_context = SDL_GL_CreateContext(g_window);
		if (g_context != nullptr)
			break;
		log("SDL_GL_CreateContext (depth %d) failed: %s", depth, SDL_GetError());
		SDL_DestroyWindow(g_window);
		g_window = nullptr;
	}
	if (g_window == nullptr || g_context == nullptr)
	{
		log("could not create an OpenGL ES 1.x window");
		return false;
	}
	g_glUp = true;

	if (SDL_GL_MakeCurrent(g_window, g_context) != 0)
	{
		log("SDL_GL_MakeCurrent failed: %s", SDL_GetError());
		return false;
	}

	// The TV compositor already paces frames through Wayland; waiting for a frame
	// callback inside every swap can hold the game to a few fps, so vsync is off
	// by default and swap_interval=1 in webos.cfg turns it back on.
	if (SDL_GL_SetSwapInterval(configInt("swap_interval", 0)) != 0)
		log("SDL_GL_SetSwapInterval failed: %s", SDL_GetError());

	SDL_GL_GetDrawableSize(g_window, &g_width, &g_height);
	if (g_width <= 0 || g_height <= 0)
	{
		SDL_GetWindowSize(g_window, &g_width, &g_height);
	}
	g_cursorX = g_width / 2;
	g_cursorY = g_height / 2;

	glDisable(GL_DITHER); // free fill-rate; 8888 needs no dithering

	// Offscreen render target + stretch-to-window. The game keeps drawing at the
	// panel size; the GPU only shades `render_scale` of those pixels.
	webos::render::scaleInit();

	SDL_StartTextInput();

	log("GL_VERSION  %s", reinterpret_cast<const char *>(glGetString(GL_VERSION)));
	log("GL_VENDOR   %s", reinterpret_cast<const char *>(glGetString(GL_VENDOR)));
	log("GL_RENDERER %s", reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
	log("drawable %dx%d", g_width, g_height);
	return true;
}

void shutdown()
{
	if (g_glUp)
	{
		webos::render::scaleShutdown();
		SDL_GL_MakeCurrent(g_window, nullptr);
		if (g_context != nullptr)
			SDL_GL_DeleteContext(g_context);
		g_context = nullptr;
		g_glUp = false;
	}
	if (g_window != nullptr)
	{
		SDL_DestroyWindow(g_window);
		g_window = nullptr;
	}
	if (g_pad.ctrl != nullptr)
	{
		SDL_GameControllerClose(g_pad.ctrl);
		g_pad = Pad();
	}
	if (g_sdlUp)
	{
		SDL_Quit();
		g_sdlUp = false;
	}
}

void makeOpaque()
{
	GLboolean mask[4];
	GLfloat color[4];
	const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
	if (scissor)
		glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(color[0], color[1], color[2], color[3]);
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
	if (scissor)
		glEnable(GL_SCISSOR_TEST);
}

void swapBuffers()
{
	// Frame time for the automatic render scale (measured frame-to-frame, so it
	// includes everything the game did plus the swap wait).
	{
		static double prevEnd = 0.0;
		const double frameNow = cpuSeconds(CLOCK_MONOTONIC);
		if (prevEnd > 0.0)
			webos::render::scaleNotifyFrameTime((frameNow - prevEnd) * 1000.0);
		prevEnd = frameNow;
	}

	// Diagnostics: GL errors for the first ~15 s, a performance report every 5 s.
	static unsigned total = 0, frames = 0;
	static double t0 = 0.0, swapWait = 0.0, mainCpu0 = 0.0, procCpu0 = 0.0, audioCpu0 = 0.0;
	static GLenum seen[16];
	static int seenCount = 0;
	// glGetError() can cost a pipeline flush; sample it instead of calling it on
	// every frame, and only while the first frames are being watched.
	static const bool checkErrors = configInt("gl_errors", 1) != 0;
	if (checkErrors && total < 900 && (total % 30u) == 0u)
	{
		GLenum e;
		for (int n = 0; n < 8 && (e = glGetError()) != GL_NO_ERROR; ++n)
		{
			bool known = false;
			for (int i = 0; i < seenCount; ++i)
				known = known || seen[i] == e;
			if (!known && seenCount < 16)
			{
				seen[seenCount++] = e;
				log("GL error 0x%04x at frame %u (first occurrence)", static_cast<unsigned>(e), total);
			}
		}
	}

	// glFinish() before the swap serialises the CPU with the tile-based GPU and
	// removes every bit of overlap between frames, so it is off by default.
	static const bool profFinish = configInt("prof_finish", 0) != 0;
	static double finishWait = 0.0;
	if (profFinish)
	{
		const double f0 = cpuSeconds(CLOCK_MONOTONIC);
		glFinish();
		finishWait += cpuSeconds(CLOCK_MONOTONIC) - f0;
	}

	// One-shot state snapshot so a transparency report can be checked against
	// what the renderer actually had enabled (alpha test / blend factors).
	if (total == 240)
	{
		GLint srcRgb = 0, dstRgb = 0, alphaFunc = 0, alphaRefFixed = 0;
		// ES 1.x has the combined GL_BLEND_SRC / GL_BLEND_DST queries.
		glGetIntegerv(GL_BLEND_SRC, &srcRgb);
		glGetIntegerv(GL_BLEND_DST, &dstRgb);
		glGetIntegerv(GL_ALPHA_TEST_FUNC, &alphaFunc);
		glGetIntegerv(GL_ALPHA_TEST_REF, &alphaRefFixed);
		log("state @frame240: alphaTest=%d blend=%d src=0x%x dst=0x%x alphaFunc=0x%x ref=%.3f depthTest=%d",
		    glIsEnabled(GL_ALPHA_TEST) ? 1 : 0, glIsEnabled(GL_BLEND) ? 1 : 0,
		    static_cast<unsigned>(srcRgb), static_cast<unsigned>(dstRgb),
		    static_cast<unsigned>(alphaFunc), alphaRefFixed / 65536.0,
		    glIsEnabled(GL_DEPTH_TEST) ? 1 : 0);
	}

	// Stretch the offscreen frame to the window. When it handled the frame it also
	// wrote alpha 1 to the window, so makeOpaque() would only cost a second pass.
	const bool presented = webos::render::scalePresent(g_width, g_height);
	static const bool opaque = configInt("opaque", 1) != 0;
	if (!presented && opaque)
		makeOpaque();

	const double s0 = cpuSeconds(CLOCK_MONOTONIC);
	if (g_window != nullptr && g_context != nullptr)
		SDL_GL_SwapWindow(g_window);
	const double now = cpuSeconds(CLOCK_MONOTONIC);
	swapWait += now - s0;

	// Hitch log: say what a >300 ms frame was doing (CPU work vs waiting on the TV).
	{
		static double lastEnd = 0.0, lastMainCpu = 0.0, lastFinish = 0.0, lastSwapWait = 0.0;
		const double mcNow = cpuSeconds(CLOCK_THREAD_CPUTIME_ID);
		if (lastEnd != 0.0 && now - lastEnd > 0.3)
			log("hitch: frame %u took %.0f ms (main cpu %.0f ms, gpu finish %.0f ms, swap %.0f ms)", total,
			    (now - lastEnd) * 1000.0, (mcNow - lastMainCpu) * 1000.0,
			    std::max(0.0, finishWait - lastFinish) * 1000.0,
			    std::max(0.0, swapWait - lastSwapWait) * 1000.0);
		lastEnd = now;
		lastMainCpu = mcNow;
		lastFinish = finishWait;
		lastSwapWait = swapWait;
	}

	++total;
	++frames;
	if (t0 == 0.0)
	{
		t0 = now;
		mainCpu0 = cpuSeconds(CLOCK_THREAD_CPUTIME_ID);
		procCpu0 = cpuSeconds(CLOCK_PROCESS_CPUTIME_ID);
		audioCpu0 = g_statAudioCpu;
	}
	if (now - t0 >= 5.0)
	{
		const double wall = now - t0;
		const double mc = cpuSeconds(CLOCK_THREAD_CPUTIME_ID);
		const double pc = cpuSeconds(CLOCK_PROCESS_CPUTIME_ID);
		log("fps %.1f (%.1f ms/frame) | main cpu %.0f%% process cpu %.0f%% audio cpu %.0f%% | "
		    "gpu finish %.1f ms, swap wait %.1f ms, fb copy %.1f ms, uploads %.1f/frame (%.0f KB, %.1f ms), %u draws/frame",
		    frames / wall, wall * 1000.0 / frames,
		    (mc - mainCpu0) / wall * 100.0, (pc - procCpu0) / wall * 100.0,
		    (g_statAudioCpu - audioCpu0) / wall * 100.0,
		    finishWait * 1000.0 / frames, swapWait * 1000.0 / frames, g_statCopyMs / frames,
		    static_cast<double>(g_statUploads) / frames, g_statUploadKB / frames, g_statUploadMs / frames,
		    g_statDraws / frames);
		frames = 0;
		t0 = now;
		swapWait = 0.0;
		finishWait = 0.0;
		g_statUploads = 0;
		g_statUploadKB = 0.0;
		g_statUploadMs = 0.0;
		mainCpu0 = mc;
		procCpu0 = pc;
		audioCpu0 = g_statAudioCpu;
		g_statCopyMs = 0.0;
		g_statDraws = 0;
	}
}

bool resizeSurface(int w, int h)
{
	if (g_window == nullptr)
		return false;
	if (w <= 0 || h <= 0)
		return false;
	SDL_SetWindowSize(g_window, w, h);
	SDL_GL_GetDrawableSize(g_window, &g_width, &g_height);
	if (g_width <= 0 || g_height <= 0)
	{
		SDL_GetWindowSize(g_window, &g_width, &g_height);
	}
	glViewport(0, 0, g_width, g_height);
	g_cursorX = g_width / 2;
	g_cursorY = g_height / 2;
	log("surface resized to %dx%d", g_width, g_height);
	return true;
}

// ------------------------------------------------------------- the pump ----
namespace
{

void handleKeyEvent(const SDL_KeyboardEvent &key, bool down)
{
	const SDL_Keycode sym = key.keysym.sym;
	const bool repeat = key.repeat != 0;

	if (g_logInput && !repeat)
		log("key sym=0x%x scancode=%d down=%d repeat=%d", static_cast<unsigned>(sym),
		    static_cast<int>(key.keysym.scancode), down ? 1 : 0, repeat ? 1 : 0);

	switch (sym)
	{
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			setMouseButton(0, down);
			return;
		case SDLK_AC_BACK:
		case SDLK_ESCAPE:
			setVKey(LK_ESCAPE, down);
			return;
		default:
			break;
	}

	// Arrows: held state for the remote-style pointer/walk handling in
	// pumpEvents(), plus the real arrow key for text fields and menus.
	int arrow = -1;
	switch (sym)
	{
		case SDLK_UP: arrow = 0; break;
		case SDLK_DOWN: arrow = 1; break;
		case SDLK_LEFT: arrow = 2; break;
		case SDLK_RIGHT: arrow = 3; break;
		default: break;
	}
	if (arrow >= 0)
	{
		g_remoteHeld[arrow] = down;
		if (!repeat)
			setVKey(keySDLToLWJGL(sym), down);
		return;
	}

	const int mapped = keyForEvent(key);
	if (mapped == 0)
		return;
	if (repeat)
	{
		if (lwjgl::Keyboard::areRepeatEventsEnabled())
			pushKey(mapped, true);
	}
	else
	{
		setVKey(mapped, down);
	}
}

void handleMouseMotion(const SDL_MouseMotionEvent &m)
{
	// The Magic Remote reports an absolute pointer position; a USB mouse reports
	// relative motion in the same event. pushMotion() stores both, so pass the
	// position through and let the shared mouse code use whichever it needs.
	const int x = std::max(0, std::min(g_width - 1, m.x));
	const int y = std::max(0, std::min(g_height - 1, m.y));
	g_cursorX = x;
	g_cursorY = y;
	lwjgl::Mouse::detail::pushMotion(x, y, m.xrel, m.yrel);
}

void handleMouseButton(const SDL_MouseButtonEvent &b)
{
	int button = 0;
	if (b.button == SDL_BUTTON_RIGHT) button = 1;
	else if (b.button == SDL_BUTTON_MIDDLE) button = 2;
	g_cursorX = std::max(0, std::min(g_width - 1, b.x));
	g_cursorY = std::max(0, std::min(g_height - 1, b.y));
	setMouseButton(button, b.type == SDL_MOUSEBUTTONDOWN);
}

// UTF-8 -> codepoints, so non-ASCII names typed on a USB keyboard reach the
// text fields instead of being dropped.
void pushUtf8(const char *text)
{
	while (text != nullptr && *text != 0)
	{
		unsigned char c = static_cast<unsigned char>(*text);
		int code = c;
		int extra = 0;
		if (c >= 0xF0) { code = c & 0x07; extra = 3; }
		else if (c >= 0xE0) { code = c & 0x0F; extra = 2; }
		else if (c >= 0xC0) { code = c & 0x1F; extra = 1; }
		++text;
		for (int i = 0; i < extra && *text != 0; ++i)
		{
			code = (code << 6) | (static_cast<unsigned char>(*text) & 0x3F);
			++text;
		}
		lwjgl::Keyboard::detail::pushChar(code);
	}
}

struct PadPrev
{
	bool a = false, b = false, x = false, y = false;
	bool lb = false, rb = false, lt = false, rt = false;
	bool back = false, start = false, l3 = false, r3 = false;
	bool up = false, down = false, left = false, right = false;
	bool chatHeld = false;
};
PadPrev g_prev;

} // namespace

void pumpEvents()
{
	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
		switch (e.type)
		{
			case SDL_QUIT:
				g_close = true;
				break;
			case SDL_WINDOWEVENT:
				switch (e.window.event)
				{
					case SDL_WINDOWEVENT_CLOSE:
						g_close = true;
						break;
					case SDL_WINDOWEVENT_HIDDEN:
					case SDL_WINDOWEVENT_MINIMIZED:
						g_focus = false;
						break;
					case SDL_WINDOWEVENT_SHOWN:
					case SDL_WINDOWEVENT_RESTORED:
					case SDL_WINDOWEVENT_FOCUS_GAINED:
						g_focus = true;
						break;
					case SDL_WINDOWEVENT_FOCUS_LOST:
						// webOS takes the focus away when the user presses Home; the
						// app may come back, so this only muting/pausing is enough.
						g_focus = false;
						break;
					case SDL_WINDOWEVENT_SIZE_CHANGED:
					case SDL_WINDOWEVENT_RESIZED:
						SDL_GL_GetDrawableSize(g_window, &g_width, &g_height);
						if (g_width <= 0 || g_height <= 0)
							SDL_GetWindowSize(g_window, &g_width, &g_height);
						clampCursor();
						break;
					default:
						break;
				}
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP:
				handleKeyEvent(e.key, e.type == SDL_KEYDOWN);
				break;
			case SDL_TEXTINPUT:
				pushUtf8(e.text.text);
				break;
			case SDL_MOUSEMOTION:
				handleMouseMotion(e.motion);
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				handleMouseButton(e.button);
				break;
			case SDL_MOUSEWHEEL:
			{
				const int delta = (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y) * 120;
				lwjgl::Mouse::detail::pushWheel(delta, g_cursorX, g_cursorY);
				break;
			}
			case SDL_CONTROLLERDEVICEADDED:
				openPad(e.cdevice.which);
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				closePad(e.cdevice.which);
				break;
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP:
			{
				if (g_pad.ctrl != nullptr && e.cbutton.which == g_pad.id &&
				    e.cbutton.button < SDL_CONTROLLER_BUTTON_MAX)
					g_btnDown[e.cbutton.button] = e.type == SDL_CONTROLLERBUTTONDOWN;
				break;
			}
			default:
				break;
		}
	}

	// frame time for rate-independent stick look / cursor movement
	static struct timespec lastTs = {0, 0};
	struct timespec nowTs;
	clock_gettime(CLOCK_MONOTONIC, &nowTs);
	float dt = 1.0f / 60.0f;
	if (lastTs.tv_sec != 0 || lastTs.tv_nsec != 0)
	{
		dt = static_cast<float>(nowTs.tv_sec - lastTs.tv_sec) +
		     static_cast<float>(nowTs.tv_nsec - lastTs.tv_nsec) * 1e-9f;
		dt = std::max(0.001f, std::min(0.1f, dt));
	}
	lastTs = nowTs;

	const bool grabbed = lwjgl::Mouse::isGrabbed();
	const float dead = std::max(0, std::min(90, configInt("deadzone", 20))) / 100.0f;
	const float lookSpeed = static_cast<float>(configInt("look_speed", 18)); // px per 1/60 s at full tilt
	const bool invertY = configInt("pad_invert_y", 0) != 0;
	const bool swapSticks = configInt("pad_swap_sticks", 0) != 0;
	const bool hatInv = configInt("hat_invert_y", 0) != 0;
	const float frames = dt * 60.0f;
	static float carryX = 0.0f, carryY = 0.0f; // sub-pixel remainder

	// ---- remote arrows ----------------------------------------------------
	// Gameplay (mouse grabbed): left/right turn the camera, up/down are walk keys
	// (the game sees the real arrow keys). GUI: all four move the pointer.
	if (grabbed)
	{
		if (g_remoteHeld[2] != g_remoteHeld[3])
			carryX += (g_remoteHeld[3] ? 1.0f : -1.0f) * lookSpeed * frames;
	}
	else if (g_remoteHeld[0] || g_remoteHeld[1] || g_remoteHeld[2] || g_remoteHeld[3])
	{
		const float step = std::max(8.0f, lookSpeed) * frames;
		carryX += ((g_remoteHeld[3] ? step : 0.0f) - (g_remoteHeld[2] ? step : 0.0f));
		carryY += ((g_remoteHeld[1] ? step : 0.0f) - (g_remoteHeld[0] ? step : 0.0f));
	}

	// ---- gamepad ----------------------------------------------------------
	float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
	if (g_pad.ctrl != nullptr)
	{
		lx = axisValue(SDL_CONTROLLER_AXIS_LEFTX);
		ly = axisValue(SDL_CONTROLLER_AXIS_LEFTY);
		rx = axisValue(SDL_CONTROLLER_AXIS_RIGHTX);
		ry = axisValue(SDL_CONTROLLER_AXIS_RIGHTY);
		if (swapSticks)
		{
			std::swap(lx, rx);
			std::swap(ly, ry);
		}
		applyDeadzone(lx, ly, dead);
		applyDeadzone(rx, ry, dead);

		// Walk with real W/A/S/D key events as well as the analog path, so the
		// stick works even if the analog snapshot is not reaching the game.
		const float t = 0.4f;
		setVKey(LK_W, grabbed && (g_remoteHeld[0] || ly < -t));
		setVKey(LK_S, grabbed && (g_remoteHeld[1] || ly > t));
		setVKey(LK_A, grabbed && lx < -t);
		setVKey(LK_D, grabbed && lx > t);
	}
	else
	{
		setVKey(LK_W, grabbed && g_remoteHeld[0]);
		setVKey(LK_S, grabbed && g_remoteHeld[1]);
		setVKey(LK_A, false);
		setVKey(LK_D, false);
	}
	g_pad.lx = lx;
	g_pad.ly = ly;
	g_pad.rx = rx;
	g_pad.ry = ry;

	auto curve = [](float v) {
		const float a = std::fabs(v);
		return v < 0 ? -a * std::sqrt(a) : a * std::sqrt(a);
	};

	if (grabbed)
	{
		carryX += curve(rx) * lookSpeed * frames;
		carryY += curve(invertY ? -ry : ry) * lookSpeed * frames;
	}
	else if (g_pad.ctrl != nullptr)
	{
		// Menus: sticks and the d-pad move the pointer. menu_speed is a percentage
		// of the old pointer speed (default 50: the pointer was too fast to aim at
		// crafting slots).
		const float speed = lookSpeed * 1.2f * frames *
		                    (std::max(5, std::min(300, configInt("menu_speed", 50))) / 100.0f);
		carryX += (curve(rx) + lx) * speed;
		carryY += (curve(ry) + ly) * speed;
		carryX += static_cast<float>(g_hatX) * speed * 0.6f;
		carryY += static_cast<float>(g_hatY) * speed * 0.6f;
	}

	const int mx = static_cast<int>(carryX);
	const int my = static_cast<int>(carryY);
	carryX -= static_cast<float>(mx);
	carryY -= static_cast<float>(my);
	moveCursor(mx, my);

	// ---- triggers: RT = attack/click, LT = use/right click ----------------
	if (g_pad.ctrl != nullptr)
	{
		const bool rt = padButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
		                axisValue(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f;
		const bool lt = padButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
		                axisValue(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f;
		if (rt != g_prev.rt) { setMouseButton(0, rt); g_prev.rt = rt; }
		if (lt != g_prev.lt) { setMouseButton(1, lt); g_prev.lt = lt; }
	}

	// ---- d-pad ------------------------------------------------------------
	if (g_pad.ctrl != nullptr)
	{
		const bool up    = SDL_GameControllerGetButton(g_pad.ctrl, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
		const bool down  = SDL_GameControllerGetButton(g_pad.ctrl, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
		const bool left  = SDL_GameControllerGetButton(g_pad.ctrl, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
		const bool right = SDL_GameControllerGetButton(g_pad.ctrl, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
		g_hatX = (right ? 1 : 0) - (left ? 1 : 0);
		g_hatY = (down ? 1 : 0) - (up ? 1 : 0);
		if (hatInv)
			g_hatY = -g_hatY;
	}

	// ---- on-screen keyboard (D-pad down opens it while a text field is focused) ----
	{
		static bool pUp = false, pDown = false, pLeft = false, pRight = false;
		static bool pA = false, pX = false, pY = false, pLB = false, pStart = false, pB = false;
		const bool up    = g_hatY < 0;
		const bool down  = g_hatY > 0;
		const bool left  = g_hatX < 0;
		const bool right = g_hatX > 0;
		const bool a = padButton(SDL_CONTROLLER_BUTTON_A);
		const bool x = padButton(SDL_CONTROLLER_BUTTON_X);
		const bool y = padButton(SDL_CONTROLLER_BUTTON_Y);
		const bool lb = padButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
		const bool st = padButton(SDL_CONTROLLER_BUTTON_START);
		const bool b = padButton(SDL_CONTROLLER_BUTTON_B);
		const bool active = oskActive();
		if (!active && !grabbed && down && !pDown && oskCanOpen())
			oskOpen();
		if (active)
		{
			oskInput((right && !pRight) - (left && !pLeft), (down && !pDown) - (up && !pUp),
			         a && !pA, x && !pX, y && !pY, lb && !pLB, st && !pStart, b && !pB);
			// swallow every pad edge this frame so nothing else (click, Escape,
			// hotbar) fires while the keyboard owns the buttons
			pUp = up; pDown = down; pLeft = left; pRight = right;
			pA = a; pX = x; pY = y; pLB = lb; pStart = st; pB = b;
			g_prevHatX = g_hatX;
			g_prevHatY = g_hatY;
			return;
		}
		pUp = up; pDown = down; pLeft = left; pRight = right;
		pA = a; pX = x; pY = y; pLB = lb; pStart = st; pB = b;
	}

	// ---- buttons ----------------------------------------------------------
	if (g_pad.ctrl != nullptr)
	{
		const bool a = padButton(SDL_CONTROLLER_BUTTON_A);
		const bool b = padButton(SDL_CONTROLLER_BUTTON_B);
		const bool x = padButton(SDL_CONTROLLER_BUTTON_X);
		const bool y = padButton(SDL_CONTROLLER_BUTTON_Y);
		const bool back = padButton(SDL_CONTROLLER_BUTTON_BACK);
		const bool start = padButton(SDL_CONTROLLER_BUTTON_START);
		const bool lb = padButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
		const bool rb = padButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
		const bool l3 = padButton(SDL_CONTROLLER_BUTTON_LEFTSTICK);
		const bool r3 = padButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK);

		// A clicks in menus, jumps in the world.
		if (a != g_prev.a)
		{
			if (!grabbed) setMouseButton(0, a);
			else setVKey(LK_SPACE, a);
			g_prev.a = a;
		}
		// B: Q (drop) in the world, Escape in menus.
		if (b != g_prev.b)
		{
			setVKey(grabbed ? LK_Q : LK_ESCAPE, b);
			g_prev.b = b;
		}
		// X / Y: inventory.
		if (x != g_prev.x) { setVKey(LK_E, x); g_prev.x = x; }
		if (y != g_prev.y) { setVKey(LK_E, y); g_prev.y = y; }
		// Start: Escape. Back: player list (Tab).
		if (start != g_prev.start) { setVKey(LK_ESCAPE, start); g_prev.start = start; }
		if (back != g_prev.back) { setVKey(LK_TAB, back); g_prev.back = back; }
		// Bumpers: hotbar prev / next.
		if (lb && !g_prev.lb) lwjgl::Mouse::detail::pushWheel(120, g_cursorX, g_cursorY);
		if (rb && !g_prev.rb) lwjgl::Mouse::detail::pushWheel(-120, g_cursorX, g_cursorY);
		g_prev.lb = lb;
		g_prev.rb = rb;
		// Stick clicks: sneak / sprint.
		if (l3 != g_prev.l3) { setVKey(LK_LSHIFT, l3); g_prev.l3 = l3; }
		if (r3 != g_prev.r3) { setVKey(LK_LCONTROL, r3); g_prev.r3 = r3; }

		if (grabbed)
		{
			// D-pad left: open chat (T), the keyboard pops up with it.
			static bool tHeld = false;
			if (tHeld) { setVKey(LK_T, false); tHeld = false; }
			if (g_hatX < 0 && g_prevHatX >= 0)
			{
				oskAutoOpenNextField();
				setVKey(LK_T, true);
				tHeld = true;
			}
			setVKey(0x3D /* F3: debug screen */, g_hatX > 0);
			const bool up = g_hatY < 0 || r3;
			static bool prevUp = false;
			if (up != prevUp)
			{
				prevUp = up;
				setVKey(0x3F /* F5 */, up);
			}
		}
	}
	g_prevHatX = g_hatX;
	g_prevHatY = g_hatY;
}

} // namespace webos

#endif // WEBOS_PLATFORM

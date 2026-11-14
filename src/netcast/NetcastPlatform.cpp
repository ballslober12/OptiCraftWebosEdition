// NetcastPlatform.cpp -- see NetcastPlatform.h.
//
// The LGNC/EGL bring-up sequence below is the one proven by the build.zip test
// harness (LGNC_SYSTEM_Initialize -> GetDisplayId -> SetResolution -> EGL window
// surface from the display id -> GLES1 context), with fallbacks added for the
// config search.
#ifdef NETCAST_PLATFORM

#include "netcast/NetcastPlatform.h"
#include "netcast/NetcastOsk.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include <unistd.h>

// lwjgl first: <linux/input.h> (pulled in by lgnc.h) #defines LK_1, LK_Q,
// LK_UP, ... which would otherwise rewrite the lwjgl::Keyboard::Key enum.
// Nothing below may name an lwjgl KEY_* enumerator; the LK_ constants are the
// same DirectInput scan codes.
#include "lwjgl/Keyboard.h"
#include "lwjgl/Mouse.h"

#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <linux/input.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <lgncopenapi/lgnc.h>
#include <lgncopenapi/lgnc_stdio.h>

// Old SDK kernel headers lack the PAD_A family; use the numeric codes.
enum
{
	PAD_A = 0x130, PAD_B = 0x131, PAD_X = 0x133, PAD_Y = 0x134,
	PAD_TL = 0x136, PAD_TR = 0x137, PAD_TL2 = 0x138, PAD_TR2 = 0x139,
	PAD_SELECT = 0x13a, PAD_START = 0x13b, PAD_THUMBL = 0x13d, PAD_THUMBR = 0x13e,
	EVK_UP = 103, EVK_LEFT = 105, EVK_RIGHT = 106, EVK_DOWN = 108,
};

// lwjgl::Keyboard::Key values (DirectInput scan codes).
enum
{
	LK_ESCAPE = 0x01, LK_1 = 0x02, LK_0 = 0x0B, LK_MINUS = 0x0C, LK_EQUALS = 0x0D,
	LK_TAB = 0x0F, LK_Q = 0x10, LK_W = 0x11, LK_E = 0x12, LK_P = 0x19,
	LK_LBRACKET = 0x1A, LK_RBRACKET = 0x1B, LK_LCONTROL = 0x1D,
	LK_A = 0x1E, LK_S = 0x1F, LK_D = 0x20, LK_L = 0x26,
	LK_SEMICOLON = 0x27, LK_APOSTROPHE = 0x28, LK_GRAVE = 0x29, LK_LSHIFT = 0x2A,
	LK_BACKSLASH = 0x2B, LK_Z = 0x2C, LK_M = 0x32, LK_COMMA = 0x33, LK_PERIOD = 0x34,
	LK_SLASH = 0x35, LK_RSHIFT = 0x36, LK_SPACE = 0x39,
	LK_NUMPADENTER = 0x9C, LK_RCONTROL = 0x9D, LK_DIVIDE = 0xB5, LK_RMENU = 0xB8,
	LK_HOME = 0xC7, LK_UP = 0xC8, LK_PRIOR = 0xC9, LK_LEFT = 0xCB, LK_RIGHT = 0xCD,
	LK_END = 0xCF, LK_DOWN = 0xD0, LK_NEXT = 0xD1, LK_INSERT = 0xD2, LK_DELETE = 0xD3,
};

#define NC_LOG(...) ::netcast::log(__VA_ARGS__)

namespace netcast
{

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
	std::FILE *f = std::fopen((g_exeDir + "netcast.cfg").c_str(), "r");
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

// ------------------------------------------------------------- EGL state ----
EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLSurface g_surf = EGL_NO_SURFACE;
EGLContext g_ctx = EGL_NO_CONTEXT;
int g_width = 1280;
int g_height = 720;
int g_dispW = 1280;
int g_dispH = 720;
bool g_lgncUp = false;

EGLConfig g_config = nullptr;
int g_window = 0;
bool g_gamepadOn = false;

std::atomic<bool> g_close(false);
std::atomic<bool> g_focus(true);

// ------------------------------------------------------- raw event queue ----
struct Raw
{
	enum Kind { Key, MouseMove, MouseButton, MouseWheel, Pad, PadHotplug } kind;
	int a, b, c, d;
};
std::mutex g_qMutex;
std::deque<Raw> g_queue;

void post(const Raw &r)
{
	std::lock_guard<std::mutex> lock(g_qMutex);
	if (g_queue.size() < 4096)
		g_queue.push_back(r);
}

// ----------------------------------------------------- key translation ----
// evdev KEY_* codes 1..88 are identical to the DirectInput scan codes LWJGL
// uses; only the extended block needs a table.
int evdevToLwjgl(unsigned code)
{
	if (code >= 1 && code <= 88)
		return static_cast<int>(code);
	switch (code)
	{
		case 96:  return LK_NUMPADENTER;
		case 97:  return LK_RCONTROL;
		case 98:  return LK_DIVIDE;
		case 100: return LK_RMENU;
		case 102: return LK_HOME;
		case 103: return LK_UP;
		case 104: return LK_PRIOR;
		case 105: return LK_LEFT;
		case 106: return LK_RIGHT;
		case 107: return LK_END;
		case 108: return LK_DOWN;
		case 109: return LK_NEXT;
		case 110: return LK_INSERT;
		case 111: return LK_DELETE;
		default:  return 0;
	}
}

// US layout character for a key, 0 when the key is not printable.
int charForKey(int key, bool shift)
{
	static const char *rowQ = "qwertyuiop";
	static const char *rowA = "asdfghjkl";
	static const char *rowZ = "zxcvbnm";
	int c = 0;
	if (key >= LK_Q && key <= LK_P)            c = rowQ[key - LK_Q];
	else if (key >= LK_A && key <= LK_L)       c = rowA[key - LK_A];
	else if (key >= LK_Z && key <= LK_M)       c = rowZ[key - LK_Z];
	if (c != 0)
		return shift ? (c - 'a' + 'A') : c;

	static const char digits[]  = "1234567890";
	static const char shifted[] = "!@#$%^&*()";
	if (key >= LK_1 && key <= LK_0)
		return shift ? shifted[key - LK_1] : digits[key - LK_1];
	switch (key)
	{
		case LK_SPACE:      return ' ';
		case LK_MINUS:      return shift ? '_' : '-';
		case LK_EQUALS:     return shift ? '+' : '=';
		case LK_LBRACKET:   return shift ? '{' : '[';
		case LK_RBRACKET:   return shift ? '}' : ']';
		case LK_SEMICOLON:  return shift ? ':' : ';';
		case LK_APOSTROPHE: return shift ? '"' : '\'';
		case LK_GRAVE:      return shift ? '~' : '`';
		case LK_BACKSLASH:  return shift ? '|' : '\\';
		case LK_COMMA:      return shift ? '<' : ',';
		case LK_PERIOD:     return shift ? '>' : '.';
		case LK_SLASH:      return shift ? '?' : '/';
		default:             return 0;
	}
}

// --------------------------------------------------- main-thread state ----
bool g_vkDown[256] = {};
bool g_shift = false;
bool g_mouseBtn[3] = {};
int  g_cursorX = 640;
int  g_cursorY = 360;
int  g_lastPtrX = -1;
int  g_lastPtrY = -1;

// remote arrows (held)
bool g_remoteHeld[4] = {}; // up, down, left, right

// gamepad state (written by pumpEvents from the queued raw events)
struct Axis
{
	int lo = 0;
	int hi = 0;
	int value = 0;
	int first = 0;
	bool seen = false;
};
Axis g_axis[64];
bool g_padBtn[0x140] = {}; // codes 0x100..0x23f
bool g_padLatch[0x140] = {};
int  g_hatX = 0;
int  g_hatY = 0;
int  g_prevHatX = 0;
int  g_prevHatY = 0;
bool g_padConnected = false;
int  g_padType = 0;
int  g_padId = -1;
float g_snap[4] = {0, 0, 0, 0};
bool g_logInput = false;

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
	if (key == LK_LSHIFT || key == LK_RSHIFT)
		g_shift = down;
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

float normalizeAxis(int code)
{
	if (code < 0 || code >= 64)
		return 0.0f;
	const Axis &a = g_axis[code];
	if (!a.seen)
		return 0.0f;
	// 8-bit pads (PS3 / generic HID) report 0..255 centred on 128; 16-bit pads
	// (xpad) report signed values. Tell them apart by the range seen so far.
	if (a.lo >= 0 && a.hi <= 255)
		return std::max(-1.0f, std::min(1.0f, (a.value - 127.5f) / 127.5f));
	return std::max(-1.0f, std::min(1.0f, a.value / 32767.0f));
}

// 0..1 for a trigger axis, whatever its raw range is.
float axisToTrigger(int code)
{
	if (code < 0 || code >= 64 || !g_axis[code].seen)
		return 0.0f;
	const Axis &a = g_axis[code];
	if (a.lo >= 0)
		return a.value / float(std::max(1, a.hi));
	return (a.value - a.lo) / float(std::max(1, a.hi - a.lo));
}

// True when an axis looks like a stick (rests mid-range) rather than a trigger
// (rests at its minimum). Signed ranges are always sticks.
bool axisLooksCentered(int code)
{
	const Axis &a = g_axis[code];
	if (!a.seen)
		return false;
	if (a.lo < 0)
		return true;
	return a.hi >= 100 && a.first > a.hi * 3 / 10 && a.first < a.hi * 7 / 10;
}

// Which axes are the right stick: xpad uses RX/RY, PS3 / generic HID pads use Z/RZ.
// netcast.cfg axis_rx / axis_ry force a choice.
void rightStickAxes(int &xc, int &yc)
{
	const int cx = configInt("axis_rx", -1), cy = configInt("axis_ry", -1);
	if (cx >= 0 && cy >= 0)
	{
		xc = cx;
		yc = cy;
		return;
	}
	xc = ABS_RX;
	yc = ABS_RY;
	if (!g_axis[ABS_RX].seen && !g_axis[ABS_RY].seen && axisLooksCentered(ABS_Z) && axisLooksCentered(ABS_RZ))
	{
		xc = ABS_Z;
		yc = ABS_RZ;
	}
}

// Left stick: ABS_X / ABS_Y unless netcast.cfg says otherwise. If those axes never
// reported, fall back to the first two stick-like axes that are not the right stick.
void leftStickAxes(int &xc, int &yc, int rxc, int ryc)
{
	const int cx = configInt("axis_lx", -1), cy = configInt("axis_ly", -1);
	if (cx >= 0 && cy >= 0)
	{
		xc = cx;
		yc = cy;
		return;
	}
	xc = ABS_X;
	yc = ABS_Y;
	if (g_axis[ABS_X].seen || g_axis[ABS_Y].seen)
		return;
	int found[2], n = 0;
	for (int c = 0; c < 8 && n < 2; ++c)
	{
		if (c == rxc || c == ryc || !axisLooksCentered(c))
			continue;
		found[n++] = c;
	}
	if (n == 2)
	{
		xc = found[0];
		yc = found[1];
	}
}

bool padButton(int code)
{
	return code >= 0x100 && code < 0x240 && g_padBtn[code - 0x100];
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

// ------------------------------------------------------- LGNC callbacks ----
// These run on LGNC's own threads: queue only.
LGNC_STATUS_T onMessage(LGNC_MSG_TYPE_T msg, unsigned int, char *, unsigned short)
{
	switch (msg)
	{
		case LGNC_MSG_FOCUS_IN:
			g_focus = true;
			break;
		case LGNC_MSG_FOCUS_OUT:
			// The app manager closes apps (Home/Exit keys, HOA_APP_ExitApp) by
			// taking the focus away rather than sending TERMINATE.
			g_focus = false;
			g_close = true;
			break;
		case LGNC_MSG_TERMINATE:
			g_close = true;
			break;
		default:
			break;
	}
	return LGNC_HANDLED;
}

unsigned int onKey(unsigned int key, LGNC_KEY_COND_T cond, LGNC_ADDITIONAL_INPUT_INFO_T *)
{
	if (cond != LGNC_KEY_PRESS && cond != LGNC_KEY_RELEASE && cond != LGNC_KEY_REPEAT)
		return LGNC_NOT_HANDLED;
	// Let the TV handle the volume keys.
	if (key == KEY_VOLUMEUP || key == KEY_VOLUMEDOWN || key == KEY_MUTE)
		return LGNC_NOT_HANDLED;
	post({Raw::Key, static_cast<int>(key), static_cast<int>(cond), 0, 0});
	return LGNC_HANDLED;
}

unsigned int onMouse(int x, int y, unsigned int key, LGNC_KEY_COND_T cond, LGNC_ADDITIONAL_INPUT_INFO_T *info)
{
	if (info != nullptr && info->event.type == EV_REL)
	{
		// Real (USB) mouse: relative motion and wheel.
		if (info->event.code == REL_X)
			post({Raw::MouseMove, 0, 0, info->event.value, 0});
		else if (info->event.code == REL_Y)
			post({Raw::MouseMove, 0, 0, 0, info->event.value});
		else if (info->event.code == REL_WHEEL)
			post({Raw::MouseWheel, info->event.value, 0, 0, 0});
		return LGNC_HANDLED;
	}
	// Magic Remote pointer: absolute position in the 1280x720 reference space.
	post({Raw::MouseMove, 1, 0, x, y});
	if (cond == LGNC_KEY_PRESS || cond == LGNC_KEY_RELEASE)
	{
		const int button = key == BTN_RIGHT ? 1 : key == BTN_MIDDLE ? 2 : 0;
		post({Raw::MouseButton, button, cond == LGNC_KEY_PRESS ? 1 : 0, 0, 0});
	}
	return LGNC_HANDLED;
}

// Delivered through both LGNC_CALLBACKS_T::joystickEventCallback and
// LGNC_GAMEPAD_RegisterCallback. Everything is applied as *state*, so seeing the
// same event through both paths is harmless.
void onPad(LGNC_ADDITIONAL_INPUT_INFO_T *e)
{
	if (e == nullptr)
		return;
	post({Raw::Pad, e->event.type, e->event.code, e->event.value, static_cast<int>(e->deviceType) | (e->deviceID << 16)});
}

void onPadHotplug(LGNC_GAMEPAD_INFO *info, int count)
{
	post({Raw::PadHotplug, count, info != nullptr ? info->id : -1, info != nullptr ? info->type : 0, 0});
}

// ------------------------------------------------------------------ EGL ----
struct EglAttempt { int r, g, b, a, depth; };

bool initEgl(int window, int msaa, int depthOverride)
{
	g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_dpy == EGL_NO_DISPLAY)
	{
		NC_LOG("eglGetDisplay failed\n");
		return false;
	}
	EGLint major = 0, minor = 0;
	if (!eglInitialize(g_dpy, &major, &minor))
	{
		NC_LOG("eglInitialize failed 0x%x\n", eglGetError());
		return false;
	}
	NC_LOG("EGL %d.%d\n", major, minor);

	eglBindAPI(EGL_OPENGL_ES_API);

	EGLint numConfigs = 0;
	if (!eglGetConfigs(g_dpy, nullptr, 0, &numConfigs) || numConfigs <= 0)
	{
		NC_LOG("eglGetConfigs found nothing");
		return false;
	}
	std::vector<EGLConfig> configs(numConfigs);

	// List everything the driver offers, so a slow or odd config can be spotted in the log.
	{
		EGLint got = 0;
		eglGetConfigs(g_dpy, configs.data(), numConfigs, &got);
		for (EGLint i = 0; i < got && i < 24; ++i)
		{
			EGLint r = 0, g = 0, b = 0, a = 0, d = 0, st = 0, cav = 0, smp = 0, rt = 0, sf = 0;
			eglGetConfigAttrib(g_dpy, configs[i], EGL_RED_SIZE, &r);
			eglGetConfigAttrib(g_dpy, configs[i], EGL_GREEN_SIZE, &g);
			eglGetConfigAttrib(g_dpy, configs[i], EGL_BLUE_SIZE, &b);
			eglGetConfigAttrib(g_dpy, configs[i], EGL_ALPHA_SIZE, &a);
			eglGetConfigAttrib(g_dpy, configs[i], EGL_DEPTH_SIZE, &d);
			eglGetConfigAttrib(g_dpy, configs[i], EGL_STENCIL_SIZE, &st);
			eglGetConfigAttrib(g_dpy, configs[i], EGL_CONFIG_CAVEAT, &cav);
			eglGetConfigAttrib(g_dpy, configs[i], EGL_SAMPLES, &smp);
			eglGetConfigAttrib(g_dpy, configs[i], EGL_RENDERABLE_TYPE, &rt);
			eglGetConfigAttrib(g_dpy, configs[i], EGL_SURFACE_TYPE, &sf);
			NC_LOG("config %2d: rgba %d%d%d%d depth %2d stencil %d samples %d caveat %s renderable 0x%x surface 0x%x", i, r, g, b, a, d, st,
			       smp, cav == EGL_NONE ? "none" : (cav == EGL_SLOW_CONFIG ? "SLOW" : "non-conformant"), rt, sf);
		}
	}

	// Pick the config exactly the way the SDL nc4 driver does (cube-2005 runs on it):
	// ask for R>=3 G>=3 B>=2 + depth, first only configs with CONFIG_CAVEAT = NONE
	// (the driver marks slow / non-conformant configs), then take the one with the
	// smallest size difference, favouring 8/8/8 truecolour.
	EGLConfig chosen = nullptr;
	const int wantDepth = depthOverride > 0 ? depthOverride : (configInt("fb16", 0) != 0 ? 16 : 24);
	const int depths[2] = { wantDepth, 16 };
	for (int pass = 0; pass < 2 && chosen == nullptr; ++pass)
	{
		for (int caveatNone = 1; caveatNone >= 0 && chosen == nullptr; --caveatNone)
		{
			EGLint attribs[32];
			int n = 0;
			attribs[n++] = EGL_RED_SIZE;   attribs[n++] = 3;
			attribs[n++] = EGL_GREEN_SIZE; attribs[n++] = 3;
			attribs[n++] = EGL_BLUE_SIZE;  attribs[n++] = 2;
			if (caveatNone)
			{
				attribs[n++] = EGL_CONFIG_CAVEAT; attribs[n++] = EGL_NONE;
			}
			attribs[n++] = EGL_DEPTH_SIZE; attribs[n++] = depths[pass];
			if (msaa > 0)
			{
				attribs[n++] = EGL_SAMPLE_BUFFERS; attribs[n++] = 1;
				attribs[n++] = EGL_SAMPLES;        attribs[n++] = msaa;
			}
			attribs[n++] = EGL_RENDERABLE_TYPE; attribs[n++] = EGL_OPENGL_ES_BIT;
			attribs[n++] = EGL_SURFACE_TYPE;    attribs[n++] = EGL_WINDOW_BIT;
			attribs[n++] = EGL_NONE;

			EGLint got = 0;
			if (!eglChooseConfig(g_dpy, attribs, configs.data(), numConfigs, &got) || got <= 0)
				continue;

			int best = -1, bestDiff = 0, bestTrue = -1, bestTrueDiff = 0;
			for (EGLint i = 0; i < got; ++i)
			{
				EGLint r = 0, g = 0, b = 0, d = 0;
				eglGetConfigAttrib(g_dpy, configs[i], EGL_RED_SIZE, &r);
				eglGetConfigAttrib(g_dpy, configs[i], EGL_GREEN_SIZE, &g);
				eglGetConfigAttrib(g_dpy, configs[i], EGL_BLUE_SIZE, &b);
				eglGetConfigAttrib(g_dpy, configs[i], EGL_DEPTH_SIZE, &d);
				const int diff = (r - 3) + (g - 3) + (b - 2) + (d - depths[pass]);
				if (best < 0 || diff < bestDiff) { best = i; bestDiff = diff; }
				if (r == 8 && g == 8 && b == 8 && (bestTrue < 0 || diff < bestTrueDiff)) { bestTrue = i; bestTrueDiff = diff; }
			}
			const int pick = bestTrue >= 0 ? bestTrue : best;
			chosen = configs[pick];
			NC_LOG("EGL config search: depth>=%d caveat-none=%d -> %d candidate(s), picked #%d", depths[pass], caveatNone, got, pick);
		}
	}
	if (chosen == nullptr)
	{
		NC_LOG("no usable EGL config");
		return false;
	}
	{
		EGLint r = 0, g = 0, b = 0, a = 0, ds = 0, ss = 0, cav = 0;
		eglGetConfigAttrib(g_dpy, chosen, EGL_RED_SIZE, &r);
		eglGetConfigAttrib(g_dpy, chosen, EGL_GREEN_SIZE, &g);
		eglGetConfigAttrib(g_dpy, chosen, EGL_BLUE_SIZE, &b);
		eglGetConfigAttrib(g_dpy, chosen, EGL_ALPHA_SIZE, &a);
		eglGetConfigAttrib(g_dpy, chosen, EGL_DEPTH_SIZE, &ds);
		eglGetConfigAttrib(g_dpy, chosen, EGL_STENCIL_SIZE, &ss);
		eglGetConfigAttrib(g_dpy, chosen, EGL_CONFIG_CAVEAT, &cav);
		NC_LOG("chosen config: rgba %d%d%d%d depth=%d stencil=%d caveat=%s", r, g, b, a, ds, ss, cav == EGL_NONE ? "none" : "SLOW/non-conformant");
	}

	// EGL_EXT_present_opaque: ignore the alpha channel when presenting.
	EGLint surfAttribs[] = { 0x31DF /* EGL_PRESENT_OPAQUE_EXT */, EGL_TRUE, EGL_NONE };
	const char *eglExt = eglQueryString(g_dpy, EGL_EXTENSIONS);
	const bool opaqueExt = eglExt != nullptr && std::strstr(eglExt, "EGL_EXT_present_opaque") != nullptr;
	NC_LOG("EGL_EXT_present_opaque: %s", opaqueExt ? "yes" : "no");
	g_config = chosen;
	g_window = window;
	g_surf = eglCreateWindowSurface(g_dpy, chosen, static_cast<EGLNativeWindowType>(static_cast<uintptr_t>(window)),
	                                opaqueExt ? surfAttribs : nullptr);
	if (g_surf == EGL_NO_SURFACE)
	{
		NC_LOG("eglCreateWindowSurface failed 0x%x\n", eglGetError());
		return false;
	}
	const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
	g_ctx = eglCreateContext(g_dpy, chosen, EGL_NO_CONTEXT, ctxAttribs);
	if (g_ctx == EGL_NO_CONTEXT)
	{
		NC_LOG("eglCreateContext failed 0x%x\n", eglGetError());
		return false;
	}
	if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx))
	{
		NC_LOG("eglMakeCurrent failed 0x%x\n", eglGetError());
		return false;
	}
	eglSwapInterval(g_dpy, configInt("swap_interval", 1));

	EGLint w = 0, h = 0;
	eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &w);
	eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &h);
	if (w > 0 && h > 0)
	{
		g_width = w;
		g_height = h;
	}
	glDisable(GL_DITHER); // free fill-rate; 8888 needs no dithering
	{
		EGLint rb = 0, sb = 0;
		eglQuerySurface(g_dpy, g_surf, EGL_RENDER_BUFFER, &rb);
		eglQuerySurface(g_dpy, g_surf, EGL_SWAP_BEHAVIOR, &sb);
		NC_LOG("surface render buffer: %s, swap behaviour: %s", rb == EGL_SINGLE_BUFFER ? "SINGLE" : "back", sb == EGL_BUFFER_PRESERVED ? "preserved" : "destroyed/undefined");
		const char *items[4] = { eglQueryString(g_dpy, EGL_VENDOR), eglQueryString(g_dpy, EGL_VERSION),
		                         eglQueryString(g_dpy, EGL_EXTENSIONS),
		                         reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS)) };
		const char *names[4] = { "EGL_VENDOR", "EGL_VERSION", "EGL_EXTENSIONS", "GL_EXTENSIONS" };
		for (int i = 0; i < 4; ++i)
		{
			const std::string str = items[i] != nullptr ? items[i] : "(null)";
			for (size_t pos = 0; pos < str.size(); pos += 180)
				NC_LOG("%s: %s", names[i], str.substr(pos, 180).c_str());
		}
	}
	NC_LOG("GL_VERSION  %s\n", reinterpret_cast<const char *>(glGetString(GL_VERSION)));
	NC_LOG("GL_RENDERER %s\n", reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
	NC_LOG("surface %d x %d\n", g_width, g_height);
	return true;
}

} // namespace

// ----------------------------------------------------------- public API ----
void log(const char *fmt, ...)
{
	char buf[1024];
	int n = std::snprintf(buf, sizeof(buf), "[netcast] ");
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
	// write(2): liblibm hooks printf/puts/fwrite on the TV and swallows them.
	(void)::write(2, buf, static_cast<size_t>(n));
	if (g_logFd == -2)
	{
		const std::string path = exeDir() + "netcast.log";
		g_logFd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	if (g_logFd >= 0)
		(void)::write(g_logFd, buf, static_cast<size_t>(n));
}

static double cpuSeconds(clockid_t id)
{
	struct timespec ts;
	clock_gettime(id, &ts);
	return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
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
	lx = g_snap[0];
	ly = g_snap[1];
	rx = g_snap[2];
	ry = g_snap[3];
	return g_padConnected;
}

static void logSystemInfo()
{
	// What is this TV actually giving us? (cpu model/speed, cores, free RAM)
	NC_LOG("cpus online: %ld", sysconf(_SC_NPROCESSORS_ONLN));
	if (std::FILE *f = std::fopen("/proc/cpuinfo", "r"))
	{
		char line[256];
		int shown = 0;
		while (std::fgets(line, sizeof(line), f) && shown < 6)
		{
			if (std::strstr(line, "model name") || std::strstr(line, "BogoMIPS") || std::strstr(line, "Features") ||
			    std::strstr(line, "CPU part") || std::strstr(line, "Hardware"))
			{
				line[std::strcspn(line, "\n")] = 0;
				NC_LOG("cpuinfo: %s", line);
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
			if (std::strstr(line, "MemTotal") || std::strstr(line, "MemFree") || std::strstr(line, "Cached:"))
			{
				line[std::strcspn(line, "\n")] = 0;
				NC_LOG("meminfo: %s", line);
			}
		}
		std::fclose(f);
	}
	// speed of a plain integer loop, for comparing with other devices
	const double t = cpuSeconds(CLOCK_MONOTONIC);
	volatile unsigned acc = 1;
	for (unsigned i = 0; i < 20000000u; ++i)
		acc = acc * 1664525u + 1013904223u;
	NC_LOG("cpu loop: 20M iterations in %.0f ms", (cpuSeconds(CLOCK_MONOTONIC) - t) * 1000.0);
}

bool initialize(int argc, char **argv)
{
	g_exeDir = resolveExeDir(argc > 0 ? argv[0] : nullptr);
	loadConfig();
	g_logInput = configInt("log_input", 0) != 0;
	NC_LOG("OptiCraft NetCast, exe dir %s", g_exeDir.c_str());
	for (int i = 0; i < argc; ++i)
		NC_LOG("argv[%d] = %s", i, argv[i]);

	NC_LOG("build %s %s", __DATE__, __TIME__);
	NC_LOG("settings: size=%dx%d swap_interval=%d opaque=%d fb16=%d use_vbo=%d copy_fb=%d sound=%d autobench=%d prof_finish=%d",
	       configInt("width", 1280), configInt("height", 720), configInt("swap_interval", 1), configInt("opaque", 1),
	       configInt("fb16", 0), configInt("use_vbo", 0), configInt("copy_fb", 0), configInt("sound", 1),
	       configInt("autobench", 1), configInt("prof_finish", 1));
	logSystemInfo();

	LGNC_CALLBACKS_T cb;
	std::memset(&cb, 0, sizeof(cb));
	cb.msgHandler            = onMessage;
	cb.keyEventCallback      = onKey;
	cb.mouseEventCallback    = onMouse;

	// LGNC_SYSTEM_Initialize makes the TV's debug framework redirect stdout/
	// stderr and hook printf & co.; undo that so the log keeps working.
	int fds[2];
	lgnc_stdio_save(fds);
	const int init = LGNC_SYSTEM_Initialize(argc, argv, &cb);
	lgnc_stdio_restore(fds);
	NC_LOG("LGNC_SYSTEM_Initialize = %d (auid 0x%llx)", init, LGNC_SYSTEM_GetAUID());
	if (init != LGNC_OK)
	{
		NC_LOG("registration failed: start the game through the app manager "
		       "(it needs \"--auid <hex>\" on the command line, see deploy/netcast/opticraft.sh)");
		return false;
	}
	g_lgncUp = true;

	int displayId = 0;
	if (LGNC_SYSTEM_GetDisplayId(&displayId) != LGNC_OK)
	{
		NC_LOG("LGNC_SYSTEM_GetDisplayId failed: the app does not have the focus");
		return false;
	}

	// 640x360 .. 1280x720 are the sizes the NC4 compositor accepts.
	// The SDL nc4 driver (cube-2005) always runs the window at 1280x720; other sizes
	// have never been measured against it, so a stale width/height in netcast.cfg is
	// ignored unless custom_size=1.
	if (configInt("custom_size", 0) != 0)
	{
		g_width  = std::max(640, std::min(1280, configInt("width", 1280)));
		g_height = std::max(360, std::min(720, configInt("height", 720)));
	}
	else
	{
		g_width = 1280;
		g_height = 720;
	}
	int r = LGNC_SYSTEM_SetResolution(static_cast<unsigned>(g_width), static_cast<unsigned>(g_height));
	NC_LOG("LGNC_SYSTEM_SetResolution(%d,%d) = %d", g_width, g_height, r);
	if (r != LGNC_OK && (g_width != 1280 || g_height != 720))
	{
		// not every size is accepted by every firmware: 1280x720 always is
		g_width = 1280;
		g_height = 720;
		r = LGNC_SYSTEM_SetResolution(1280, 720);
		NC_LOG("fallback LGNC_SYSTEM_SetResolution(1280,720) = %d", r);
	}
	if (r != LGNC_OK)
		return false;

	if (!initEgl(displayId, configInt("msaa", 0), configInt("depth", 0)))
		return false;

	// hide LGLaunchImage.png, shown by LGNC_SYSTEM_Initialize()
	LGNC_SYSTEM_RemoveLaunchImage();

	g_cursorX = g_width / 2;
	g_cursorY = g_height / 2;

	return true;
}

void enableGamepad()
{
	if (g_gamepadOn)
		return;
	const int gp = LGNC_GAMEPAD_RegisterCallback(onPad, onPadHotplug);
	NC_LOG("LGNC_GAMEPAD_RegisterCallback = %d", gp);
	g_gamepadOn = gp == LGNC_OK;
}

void disableGamepad()
{
	if (!g_gamepadOn)
		return;
	LGNC_GAMEPAD_UnregisterCallback();
	g_gamepadOn = false;
	NC_LOG("gamepad API unregistered");
}

bool resizeSurface(int w, int h)
{
	if (g_dpy == EGL_NO_DISPLAY || g_ctx == EGL_NO_CONTEXT)
		return false;
	const int oldW = g_width, oldH = g_height;
	eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (g_surf != EGL_NO_SURFACE)
		eglDestroySurface(g_dpy, g_surf);
	g_surf = EGL_NO_SURFACE;

	int sizes[2][2] = { { w, h }, { oldW, oldH } };
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		const int r = LGNC_SYSTEM_SetResolution(static_cast<unsigned>(sizes[attempt][0]), static_cast<unsigned>(sizes[attempt][1]));
		NC_LOG("LGNC_SYSTEM_SetResolution(%d,%d) = %d", sizes[attempt][0], sizes[attempt][1], r);
		if (r != LGNC_OK)
			continue;
		g_surf = eglCreateWindowSurface(g_dpy, g_config, static_cast<EGLNativeWindowType>(static_cast<uintptr_t>(g_window)), nullptr);
		if (g_surf == EGL_NO_SURFACE)
		{
			NC_LOG("eglCreateWindowSurface failed 0x%x", eglGetError());
			continue;
		}
		if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx))
		{
			NC_LOG("eglMakeCurrent failed 0x%x", eglGetError());
			eglDestroySurface(g_dpy, g_surf);
			g_surf = EGL_NO_SURFACE;
			continue;
		}
		eglSwapInterval(g_dpy, configInt("swap_interval", 1));
		EGLint qw = 0, qh = 0;
		eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &qw);
		eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &qh);
		g_width = qw > 0 ? qw : sizes[attempt][0];
		g_height = qh > 0 ? qh : sizes[attempt][1];
		glViewport(0, 0, g_width, g_height);
		g_cursorX = g_width / 2;
		g_cursorY = g_height / 2;
		return attempt == 0;
	}
	return false;
}

void shutdown()
{
	if (g_dpy != EGL_NO_DISPLAY)
	{
		eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (g_ctx != EGL_NO_CONTEXT)  eglDestroyContext(g_dpy, g_ctx);
		if (g_surf != EGL_NO_SURFACE) eglDestroySurface(g_dpy, g_surf);
		eglTerminate(g_dpy);
		g_dpy = EGL_NO_DISPLAY;
		g_ctx = EGL_NO_CONTEXT;
		g_surf = EGL_NO_SURFACE;
	}
	if (g_lgncUp)
	{
		LGNC_GAMEPAD_UnregisterCallback();
		// reports the exit to the app manager (HOA_APP_ExitAppEx)
		LGNC_SYSTEM_Finalize();
		g_lgncUp = false;
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
	// Diagnostics: GL errors for the first ~25 s, a performance report every 5 s.
	static unsigned total = 0, frames = 0;
	static double t0 = 0.0, swapWait = 0.0, mainCpu0 = 0.0, procCpu0 = 0.0, audioCpu0 = 0.0;
	static GLenum seen[16];
	static int seenCount = 0;
	if (total < 1500)
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
				NC_LOG("GL error 0x%04x at frame %u (first occurrence)", static_cast<unsigned>(e), total);
			}
		}
	}

	// Profiling aid: waiting for the GPU here separates "the GPU is busy rendering"
	// (finish wait) from "the display is pacing us" (swap wait below).
	static const bool profFinish = configInt("prof_finish", 1) != 0;
	static double finishWait = 0.0;
	if (profFinish)
	{
		const double f0 = cpuSeconds(CLOCK_MONOTONIC);
		glFinish();
		finishWait += cpuSeconds(CLOCK_MONOTONIC) - f0;
	}
	static const bool opaque = configInt("opaque", 1) != 0;
	if (opaque)
		makeOpaque();

	const double s0 = cpuSeconds(CLOCK_MONOTONIC);
	if (g_dpy != EGL_NO_DISPLAY && g_surf != EGL_NO_SURFACE)
		eglSwapBuffers(g_dpy, g_surf);
	const double now = cpuSeconds(CLOCK_MONOTONIC);
	swapWait += now - s0;

	// Hitch log: say what a >300 ms frame was doing (CPU work vs waiting on the TV).
	{
		static double lastEnd = 0.0, lastMainCpu = 0.0, lastFinish = 0.0, lastSwapWait = 0.0;
		const double mcNow = cpuSeconds(CLOCK_THREAD_CPUTIME_ID);
		if (lastEnd != 0.0 && now - lastEnd > 0.3)
			NC_LOG("hitch: frame %u took %.0f ms (main cpu %.0f ms, gpu finish %.0f ms, swap %.0f ms)", total,
			       (now - lastEnd) * 1000.0, (mcNow - lastMainCpu) * 1000.0, std::max(0.0, finishWait - lastFinish) * 1000.0,
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
		NC_LOG("fps %.1f (%.1f ms/frame) | main cpu %.0f%% process cpu %.0f%% audio cpu %.0f%% | "
		       "gpu finish %.1f ms, swap wait %.1f ms, fb copy %.1f ms, uploads %.1f/frame (%.0f KB, %.1f ms), %u draws/frame",
		       frames / wall, wall * 1000.0 / frames,
		       (mc - mainCpu0) / wall * 100.0, (pc - procCpu0) / wall * 100.0,
		       (g_statAudioCpu - audioCpu0) / wall * 100.0,
		       finishWait * 1000.0 / frames, swapWait * 1000.0 / frames, g_statCopyMs / frames,
		       static_cast<double>(g_statUploads) / frames, g_statUploadKB / frames, g_statUploadMs / frames,
		       g_statDraws / frames);
		{
			// memory: the TV has ~720 MB and swaps; a growing RSS explains slowdowns over time
			long rssKb = 0, freeKb = 0, swapFreeKb = 0;
			if (FILE *f = std::fopen("/proc/self/status", "r"))
			{
				char line[160];
				while (std::fgets(line, sizeof(line), f))
					if (std::sscanf(line, "VmRSS: %ld kB", &rssKb) == 1) break;
				std::fclose(f);
			}
			if (FILE *f = std::fopen("/proc/meminfo", "r"))
			{
				char line[160];
				while (std::fgets(line, sizeof(line), f))
				{
					std::sscanf(line, "MemFree: %ld kB", &freeKb);
					std::sscanf(line, "SwapFree: %ld kB", &swapFreeKb);
				}
				std::fclose(f);
			}
			NC_LOG("mem: rss %ld MB, system free %ld MB, swap free %ld MB", rssKb / 1024, freeKb / 1024, swapFreeKb / 1024);
		}
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

// ------------------------------------------------------------- the pump ----
namespace
{

// Remote-control and keyboard keys. Returns after handling.
void handleRawKey(int code, int cond)
{
	const bool down = cond != LGNC_KEY_RELEASE;
	const bool repeat = cond == LGNC_KEY_REPEAT;

	if (g_logInput)
		NC_LOG("key code=%d cond=%d", code, cond);

	switch (code)
	{
		case LGNC_REMOTE_PRESS: // Magic Remote click (BTN_LEFT)
		case KEY_OK:            // plain remote OK
			setMouseButton(0, down); return;
		case LGNC_REMOTE_BACK:  // KEY_PREVIOUS, the remote's Back key
		case KEY_BACK:
		case KEY_EXIT:
			setVKey(LK_ESCAPE, down); return;
		case LGNC_REMOTE_RED:    setMouseButton(1, down); return;   // use / place
		case LGNC_REMOTE_GREEN:  setVKey(LK_SPACE, down); return;   // jump
		case LGNC_REMOTE_YELLOW: setVKey(LK_E, down); return;       // inventory
		case LGNC_REMOTE_BLUE:   setVKey(LK_LSHIFT, down); return;  // sneak
		case LGNC_REMOTE_CH_UP:
			if (down && !repeat) lwjgl::Mouse::detail::pushWheel(120, g_cursorX, g_cursorY);
			return;
		case LGNC_REMOTE_CH_DOWN:
			if (down && !repeat) lwjgl::Mouse::detail::pushWheel(-120, g_cursorX, g_cursorY);
			return;
		default: break;
	}
	if (code >= LGNC_REMOTE_NUM_1 && code <= LGNC_REMOTE_NUM_10)
	{
		const int n = code - LGNC_REMOTE_NUM_1; // 0..9
		setVKey(n == 9 ? LK_0 : LK_1 + n, down);
		return;
	}

	// Arrow keys (remote or USB keyboard): held state for the remote-style
	// pointer/walk handling in pumpEvents(), plus the real arrow key for text
	// fields and menus.
	int arrow = -1;
	if (code == EVK_UP) arrow = 0;
	else if (code == EVK_DOWN) arrow = 1;
	else if (code == EVK_LEFT) arrow = 2;
	else if (code == EVK_RIGHT) arrow = 3;
	if (arrow >= 0)
	{
		g_remoteHeld[arrow] = down;
		if (!repeat)
			setVKey(evdevToLwjgl(static_cast<unsigned>(code)), down);
		return;
	}

	const int key = evdevToLwjgl(static_cast<unsigned>(code));
	if (key == 0)
		return;
	if (repeat)
	{
		if (lwjgl::Keyboard::areRepeatEventsEnabled())
			pushKey(key, true);
	}
	else
	{
		setVKey(key, down);
	}
	if (down)
	{
		const int ch = charForKey(key, g_shift);
		if (ch != 0)
			lwjgl::Keyboard::detail::pushChar(ch);
	}
}

void handleRawPad(int type, int code, int value, int devInfo)
{
	g_padType = devInfo & 0xffff;
	g_padId = devInfo >> 16;
	g_padConnected = true;
	static int s_padLogged = 0;
	if (g_logInput || s_padLogged < 150)
	{
		++s_padLogged;
		NC_LOG("pad dev=%d#%d ev type=%d code=%d value=%d", g_padType, g_padId, type, code, value);
	}

	if (type == EV_ABS)
	{
		if (code == ABS_HAT0X) { g_hatX = value; return; }
		if (code == ABS_HAT0Y) { g_hatY = value; return; }
		if (code >= 0 && code < 64)
		{
			Axis &a = g_axis[code];
			if (!a.seen)
			{
				a.seen = true;
				a.first = value;
				a.lo = std::min(value, 0);
				a.hi = std::max(value, 0);
			}
			a.lo = std::min(a.lo, value);
			a.hi = std::max(a.hi, value);
			a.value = value;
		}
	}
	else if (type == EV_KEY)
	{
		if (code >= 0x100 && code < 0x240)
		{
			// This TV's pad reports each tap as value 0 then 1 (active-low): treating
			// 1 as "down" left buttons stuck after a single press. btn_invert=0 restores
			// the usual meaning.
			static const bool invert = configInt("btn_invert", 1) != 0;
			const bool down = invert ? (value == 0) : (value != 0);
			g_padBtn[code - 0x100] = down;
			if (down)
				g_padLatch[code - 0x100] = true; // a tap that starts and ends within one frame still counts
		}
	}
}

// Button -> action table. Defaults follow the Linux xpad layout; every code can
// be overridden in netcast.cfg (btn_jump=304 ...) after watching log_input=1.
struct PadMap
{
	int jump, inventory, inventory2, drop, pause, sneak, sprint, playerList, prev, next, back;
};

PadMap loadPadMap()
{
	PadMap m;
	m.jump       = configInt("btn_jump",      PAD_A);
	m.back       = configInt("btn_back",      PAD_B);      // game: Q (drop), menus: Escape
	m.inventory  = configInt("btn_inventory", PAD_X);      // E
	m.inventory2 = configInt("btn_inventory2", PAD_Y);     // E
	m.drop       = configInt("btn_drop",      -1);         // unused: B is Q now
	m.pause      = configInt("btn_pause",     PAD_START);
	m.playerList = configInt("btn_playerlist", PAD_SELECT);
	m.sneak      = configInt("btn_sneak",     PAD_THUMBL);
	m.sprint     = configInt("btn_sprint",    -1);          // R3 is F5 now
	m.prev       = configInt("btn_prev",      PAD_TL);
	m.next       = configInt("btn_next",      PAD_TR);
	return m;
}

float lookCurve(float v)
{
	// finer aim near the centre, full speed at the edge
	const float a = std::fabs(v);
	return v < 0 ? -a * std::sqrt(a) : a * std::sqrt(a);
}

} // namespace

void pumpEvents()
{
	std::deque<Raw> local;
	{
		std::lock_guard<std::mutex> lock(g_qMutex);
		local.swap(g_queue);
	}

	for (const Raw &r : local)
	{
		switch (r.kind)
		{
			case Raw::Key:
				handleRawKey(r.a, r.b);
				break;
			case Raw::MouseMove:
				if (r.a == 1)
				{
					// absolute pointer: 1280x720 reference space -> surface space
					const int x = std::max(0, std::min(g_width - 1, r.c * g_width / 1280));
					const int y = std::max(0, std::min(g_height - 1, r.d * g_height / 720));
					lwjgl::Mouse::detail::pushMotion(x, y, g_lastPtrX >= 0 ? x - g_lastPtrX : 0, g_lastPtrX >= 0 ? y - g_lastPtrY : 0);
					g_lastPtrX = g_cursorX = x;
					g_lastPtrY = g_cursorY = y;
				}
				else
				{
					moveCursor(r.c, r.d);
				}
				break;
			case Raw::MouseButton:
				setMouseButton(r.a, r.b != 0);
				break;
			case Raw::MouseWheel:
				lwjgl::Mouse::detail::pushWheel(r.a * 120, g_cursorX, g_cursorY);
				break;
			case Raw::Pad:
				handleRawPad(r.a, r.b, r.c, r.d);
				break;
			case Raw::PadHotplug:
				NC_LOG("gamepad hotplug: count=%d id=%d type=%d", r.a, r.b, r.c);
				g_padConnected = r.a > 0;
				if (!g_padConnected)
				{
					std::memset(g_padBtn, 0, sizeof(g_padBtn));
					for (Axis &a : g_axis) a = Axis();
					g_hatX = g_hatY = 0;
				}
				break;
		}
	}

	// frame time for rate-independent stick look
	static struct timespec lastTs = {0, 0};
	struct timespec nowTs;
	clock_gettime(CLOCK_MONOTONIC, &nowTs);
	float dt = 1.0f / 60.0f;
	if (lastTs.tv_sec != 0 || lastTs.tv_nsec != 0)
	{
		dt = static_cast<float>(nowTs.tv_sec - lastTs.tv_sec) + static_cast<float>(nowTs.tv_nsec - lastTs.tv_nsec) * 1e-9f;
		dt = std::max(0.001f, std::min(0.1f, dt));
	}
	lastTs = nowTs;

	const bool grabbed = lwjgl::Mouse::isGrabbed();
	const float dead = std::max(0, std::min(90, configInt("deadzone", 20))) / 100.0f;
	const float lookSpeed = static_cast<float>(configInt("look_speed", 18)); // px per 1/60 s at full tilt
	const bool invertY = configInt("pad_invert_y", 0) != 0;
	const bool swapSticks = configInt("pad_swap_sticks", 0) != 0;
	const float frames = dt * 60.0f;
	static float carryX = 0.0f, carryY = 0.0f; // sub-pixel remainder

	// ---- remote arrows ----------------------------------------------------
	// Gameplay (mouse grabbed): up/down walk, left/right turn the camera.
	// GUI: all four move the pointer.
	if (grabbed)
	{
		if (g_remoteHeld[2] != g_remoteHeld[3])
		{
			carryX += (g_remoteHeld[3] ? 1.0f : -1.0f) * lookSpeed * frames;
		}
	}
	else if (g_remoteHeld[0] || g_remoteHeld[1] || g_remoteHeld[2] || g_remoteHeld[3])
	{
		const float step = std::max(8.0f, lookSpeed) * frames;
		carryX += ((g_remoteHeld[3] ? step : 0.0f) - (g_remoteHeld[2] ? step : 0.0f));
		carryY += ((g_remoteHeld[1] ? step : 0.0f) - (g_remoteHeld[0] ? step : 0.0f));
	}

	// ---- gamepad ----------------------------------------------------------
	int rxc = ABS_RX, ryc = ABS_RY;
	rightStickAxes(rxc, ryc);
	const bool rightOnZ = rxc == ABS_Z || ryc == ABS_RZ;
	int lxc = ABS_X, lyc = ABS_Y;
	leftStickAxes(lxc, lyc, rxc, ryc);
	float lx = normalizeAxis(lxc), ly = normalizeAxis(lyc);
	float rx = normalizeAxis(rxc), ry = normalizeAxis(ryc);
	if (swapSticks)
	{
		std::swap(lx, rx);
		std::swap(ly, ry);
	}
	applyDeadzone(lx, ly, dead);
	applyDeadzone(rx, ry, dead);
	// Walk with real W/A/S/D key events as well as the analog path, so the stick
	// works even if the analog snapshot is not reaching the game.
	{
		const float t = 0.4f;
		const bool walk = grabbed;
		setVKey(LK_W, walk && (g_remoteHeld[0] || ly < -t));
		setVKey(LK_S, walk && (g_remoteHeld[1] || ly > t));
		setVKey(LK_A, walk && lx < -t);
		setVKey(LK_D, walk && lx > t);
	}
	g_snap[0] = lx;
	g_snap[1] = ly;
	g_snap[2] = rx;
	g_snap[3] = ry;

	if (grabbed)
	{
		// Right stick looks (fed through the mouse path, like a real mouse).
		// Left stick walks through platformGamepadSnapshot(), not through keys.
		carryX += lookCurve(rx) * lookSpeed * frames;
		carryY += lookCurve(invertY ? -ry : ry) * lookSpeed * frames;
	}
	else
	{
		// Menus: sticks and the d-pad move the pointer.
		// menu_speed is a percentage of the old pointer speed (default 50: the
		// pointer was too fast to aim at crafting slots).
		const float speed = lookSpeed * 1.2f * frames * (std::max(5, std::min(300, configInt("menu_speed", 50))) / 100.0f);
		carryX += (lookCurve(rx) + lx) * speed;
		carryY += (lookCurve(ry) + ly) * speed;
		carryX += static_cast<float>(g_hatX) * speed * 0.6f;
		carryY += static_cast<float>(g_hatY) * speed * 0.6f;
	}

	const int mx = static_cast<int>(carryX);
	const int my = static_cast<int>(carryY);
	carryX -= static_cast<float>(mx);
	carryY -= static_cast<float>(my);
	moveCursor(mx, my);

	static const PadMap map = loadPadMap();

	// ---- on-screen keyboard (D-pad down opens it while a text field is focused) ----
	{
		static bool pUp = false, pDown = false, pLeft = false, pRight = false;
		static bool pA = false, pX = false, pY = false, pLB = false, pStart = false, pB = false;
		static const bool hatInv = configInt("hat_invert_y", 0) != 0;
		const bool up    = (hatInv ? g_hatY > 0 : g_hatY < 0) || padButton(0x220);
		const bool down  = (hatInv ? g_hatY < 0 : g_hatY > 0) || padButton(0x221);
		const bool left  = g_hatX < 0 || padButton(0x222);
		const bool right = g_hatX > 0 || padButton(0x223);
		const bool a = padButton(map.jump), x = padButton(map.inventory), y = padButton(map.inventory2);
		const bool lb = padButton(map.prev), st = padButton(map.pause), b = padButton(map.back);
		const bool active = oskActive();
		if (!active && !grabbed && down && !pDown && oskCanOpen())
			oskOpen();
		if (active)
		{
			oskInput((right && !pRight) - (left && !pLeft), (down && !pDown) - (up && !pUp),
			         a && !pA, x && !pX, y && !pY, lb && !pLB, st && !pStart, b && !pB);
			// swallow every pad edge this frame so nothing else (click, Escape, hotbar) fires
			pUp = up; pDown = down; pLeft = left; pRight = right;
			pA = a; pX = x; pY = y; pLB = lb; pStart = st; pB = b;
			for (const int btn : { map.jump, map.back, map.inventory, map.inventory2, map.pause, map.prev, map.next })
				if (btn >= 0x100 && btn < 0x240) { g_padLatch[btn - 0x100] = false; }
			g_prevHatX = g_hatX;
			g_prevHatY = g_hatY;
			return;
		}
		pUp = up; pDown = down; pLeft = left; pRight = right;
		pA = a; pX = x; pY = y; pLB = lb; pStart = st; pB = b;
	}
	const bool rt = padButton(PAD_TR2) || (!rightOnZ && axisToTrigger(ABS_RZ) > 0.5f);
	const bool lt = padButton(PAD_TL2) || (!rightOnZ && axisToTrigger(ABS_Z) > 0.5f);
	static bool prevRt = false, prevLt = false;
	if (rt != prevRt) { setMouseButton(0, rt); prevRt = rt; } // attack / click
	if (lt != prevLt) { setMouseButton(1, lt); prevLt = lt; } // use / place

	static bool prevBtn[0x140] = {};
	struct Bind { int btn; int key; bool menuClick; bool menuEscape; };
	const Bind binds[] = {
		{ map.jump,       LK_SPACE,    true,  false },
		{ map.back,       LK_Q,        false, true  },   // B: Q in game, Escape in menus
		{ map.inventory,  LK_E,        false, false },   // X: E
		{ map.inventory2, LK_E,        false, false },   // Y: E
		{ map.pause,      LK_ESCAPE,   false, false },
		{ map.playerList, LK_TAB,      false, false },
		{ map.sneak,      LK_LSHIFT,   false, false },
		{ map.sprint,     LK_LCONTROL, false, false },
	};
	for (const Bind &b : binds)
	{
		if (b.btn < 0x100 || b.btn >= 0x240)
			continue;
		bool &latch = g_padLatch[b.btn - 0x100];
		const bool now = padButton(b.btn) || latch;
		latch = false;
		if (now == prevBtn[b.btn - 0x100])
			continue;
		prevBtn[b.btn - 0x100] = now;
		// Remember what the press did so the release undoes the same thing even if
		// the GUI opened/closed in between (otherwise Escape / the click stays stuck).
		static int pressAction[0x140] = {}; // 0 none, 1 click, 2 escape, 3 key
		int &act = pressAction[b.btn - 0x100];
		if (now)
			act = (!grabbed && b.menuClick) ? 1 : ((!grabbed && b.menuEscape) ? 2 : (b.key != 0 ? 3 : 0));
		if (act == 1)
			setMouseButton(0, now);                 // A clicks in menus
		else if (act == 2)
			setVKey(LK_ESCAPE, now);                // B closes menus
		else if (act == 3)
			setVKey(b.key, now);
		if (!now)
			act = 0;
	}

	// bumpers and d-pad left/right: hotbar; d-pad up: change view
	static bool prevPrev = false, prevNext = false, prevUp = false;
	static bool tHeld = false;
	if (tHeld) { setVKey(0x14, false); tHeld = false; }  // T tap lasts one frame
	const bool bp = padButton(map.prev), bn = padButton(map.next);
	if (bp && !prevPrev) lwjgl::Mouse::detail::pushWheel(120, g_cursorX, g_cursorY);
	if (bn && !prevNext) lwjgl::Mouse::detail::pushWheel(-120, g_cursorX, g_cursorY);
	prevPrev = bp;
	prevNext = bn;
	if (grabbed)
	{
		// D-pad left: open chat (T), keyboard pops up with it
		if (!tHeld && g_hatX < 0 && g_prevHatX >= 0)
		{
			oskAutoOpenNextField();
			setVKey(0x14, true);
			tHeld = true;
		}
		setVKey(0x3D /* F3: debug screen */, g_hatX > 0 || padButton(0x223));
		static const bool hatInvertY = configInt("hat_invert_y", 0) != 0;
		const bool up = (hatInvertY ? g_hatY > 0 : g_hatY < 0) || padButton(0x220) || padButton(PAD_THUMBR); // D-pad up / R3 = F5
		if (up != prevUp)
		{
			prevUp = up;
			setVKey(0x3F /* F5 */, up);
		}
	}
	g_prevHatX = g_hatX;
	g_prevHatY = g_hatY;
}

} // namespace netcast

#endif // NETCAST_PLATFORM

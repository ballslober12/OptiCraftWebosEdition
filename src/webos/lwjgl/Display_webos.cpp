// Display_webos.cpp -- lwjgl::Display on top of the SDL2 window.
// The window and the GL context already exist (webos::initialize, called from
// main), so create() does nothing; processMessages() is where SDL events are
// pumped into the lwjgl keyboard/mouse queues.
#ifdef WEBOS_PLATFORM

#include "lwjgl/Display.h"
#include "webos/WebOSPlatform.h"

namespace lwjgl
{
namespace Display
{

void setDisplayMode(const DisplayMode &mode)
{
	// The TV compositor scales the window to the panel; a mode change only
	// shrinks the render target.
	webos::resizeSurface(static_cast<int>(mode.getWidth()), static_cast<int>(mode.getHeight()));
}

DisplayMode getDisplayMode() { return DisplayMode(webos::width(), webos::height()); }

void setTitle(const jstring &) {}
void setFullscreen(bool) {} // the compositor decides

bool isCloseRequested() { return webos::closeRequested(); }
bool isVisible() { return true; }
bool isActive() { return webos::hasFocus(); }

void processMessages() { webos::pumpEvents(); }
void swapBuffers() { webos::swapBuffers(); }

void update(bool doProcessMessages)
{
	swapBuffers();
	if (doProcessMessages)
		processMessages();
}

void create() {}

int_t getX() { return 0; }
int_t getY() { return 0; }
int_t getWidth() { return webos::width(); }
int_t getHeight() { return webos::height(); }

} // namespace Display
} // namespace lwjgl

#endif // WEBOS_PLATFORM

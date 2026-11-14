// Display_netcast.cpp -- lwjgl::Display on top of the LGNC window + EGL surface.
// The window already exists (NetcastPlatform::initialize, called from main), so
// there is nothing to create; processMessages() is where LGNC input is pumped
// into the lwjgl keyboard/mouse queues.
#ifdef NETCAST_PLATFORM

#include "lwjgl/Display.h"
#include "netcast/NetcastPlatform.h"

namespace lwjgl
{
namespace Display
{

void setDisplayMode(const DisplayMode &) {} // the TV compositor decides
DisplayMode getDisplayMode() { return DisplayMode(netcast::width(), netcast::height()); }

void setTitle(const jstring &) {}
void setFullscreen(bool) {}

bool isCloseRequested() { return netcast::closeRequested(); }
bool isVisible() { return true; }
bool isActive() { return netcast::hasFocus(); }

void processMessages() { netcast::pumpEvents(); }
void swapBuffers() { netcast::swapBuffers(); }

void update(bool doProcessMessages)
{
	swapBuffers();
	if (doProcessMessages)
		processMessages();
}

void create() {}

int_t getX() { return 0; }
int_t getY() { return 0; }
int_t getWidth() { return netcast::width(); }
int_t getHeight() { return netcast::height(); }

} // namespace Display
} // namespace lwjgl

#endif // NETCAST_PLATFORM

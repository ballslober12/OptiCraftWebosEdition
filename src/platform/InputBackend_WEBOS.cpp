// InputBackend_WEBOS.cpp -- controller input for LG webOS.
//
// Menus and buttons reach the game through the lwjgl keyboard/mouse queues (see
// webos/WebOSPlatform.cpp). What this backend adds is the *analog* part: with
// PLATFORM_DIRECT_ANALOG_MOVEMENT the game reads the left stick here to walk
// smoothly instead of through W/A/S/D.
#include "platform/Input.h"
#include "webos/WebOSPlatform.h"

PlatformTextInputSnapshot platformTextInputSnapshot(int)
{
    // Text arrives as SDL_TEXTINPUT events plus the in-game on-screen keyboard;
    // there is no separate per-port text snapshot on this platform.
    return {};
}

PlatformGamepadSnapshot platformGamepadSnapshot(int port)
{
    PlatformGamepadSnapshot snap;
    if (port != 0)
        return snap;
    snap.connected = webos::padSnapshot(snap.leftX, snap.leftY, snap.rightX, snap.rightY);
    return snap;
}

PlatformGamepadSnapshot platformRawGamepadSnapshot(int port)
{
    return platformGamepadSnapshot(port);
}

int platformMenuPad() { return 0; }
bool platformMenuPointerActive() { return true; }
bool platformMenuCursorVisible() { return false; }
void platformSetMenuCursor(int, int) {}

const PlatformKeyboardHints& platformKeyboardHints()
{
    static const PlatformKeyboardHints hints;
    return hints;
}

const char* platformInputDebugLine() { return ""; }

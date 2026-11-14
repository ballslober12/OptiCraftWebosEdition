// InputBackend_NETCAST.cpp -- controller input for LG NetCast.
//
// Menus and buttons reach the game through the lwjgl keyboard/mouse queues (see
// netcast/NetcastPlatform.cpp). What this backend adds is the *analog* part:
// with PLATFORM_DIRECT_ANALOG_MOVEMENT the game reads the left stick here to
// walk smoothly instead of through W/A/S/D.
#include "platform/Input.h"
#include "netcast/NetcastPlatform.h"

PlatformTextInputSnapshot platformTextInputSnapshot(int)
{
    return {};
}

PlatformGamepadSnapshot platformGamepadSnapshot(int port)
{
    PlatformGamepadSnapshot snap;
    if (port != 0)
        return snap;
    snap.connected = netcast::padSnapshot(snap.leftX, snap.leftY, snap.rightX, snap.rightY);
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

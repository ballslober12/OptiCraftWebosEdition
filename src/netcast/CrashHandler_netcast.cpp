// CrashHandler_netcast.cpp -- no message boxes on the TV: the crash text goes to
// netcast.log (and stderr), which is where it can be read back from the USB stick.
#ifdef NETCAST_PLATFORM

#include "pc/CrashHandler.h"
#include "netcast/NetcastPlatform.h"

namespace CrashHandler
{

void Crash(const std::string &message, const std::string &stackTrace)
{
	netcast::log("CRASH: %s", message.c_str());
	if (!stackTrace.empty())
		netcast::log("%s", stackTrace.c_str());
}

} // namespace CrashHandler

#endif // NETCAST_PLATFORM

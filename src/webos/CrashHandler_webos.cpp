// CrashHandler_webos.cpp -- no message boxes on the TV: the crash text goes to
// webos.log (and stderr), which is where it can be read back over the dev-mode
// connection (or from the app folder).
#ifdef WEBOS_PLATFORM

#include "pc/CrashHandler.h"
#include "webos/WebOSPlatform.h"

namespace CrashHandler
{

void Crash(const std::string &message, const std::string &stackTrace)
{
	webos::log("CRASH: %s", message.c_str());
	if (!stackTrace.empty())
		webos::log("%s", stackTrace.c_str());
}

} // namespace CrashHandler

#endif // WEBOS_PLATFORM

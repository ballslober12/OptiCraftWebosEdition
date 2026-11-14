// GLContext_webos.cpp -- the ES 1.x context is created by webos::initialize();
// this only records the GL extension list so capability checks work.
#ifdef WEBOS_PLATFORM

#include "lwjgl/GLContext.h"
#include "platform/RenderAPI.h"

#include <string>

namespace lwjgl
{
namespace GLContext
{
namespace detail
{
static GLCapabilities &webosCaps()
{
	static GLCapabilities caps;
	return caps;
}
} // namespace detail

void setRequestedSamples(int) {}
int getRequestedSamples() { return 0; }

void instantiate()
{
	const unsigned char *ext = renderGetString(RenderStringQuery::Extensions);
	if (ext == nullptr)
		return;
	std::string cap;
	for (const char *p = reinterpret_cast<const char *>(ext);; ++p)
	{
		if (*p == ' ' || *p == '\0')
		{
			if (!cap.empty())
				detail::webosCaps().add(cap);
			cap.clear();
			if (*p == '\0')
				break;
		}
		else
		{
			cap.push_back(*p);
		}
	}
}

const detail::GLCapabilities &getCapabilities()
{
	return detail::webosCaps();
}

} // namespace GLContext
} // namespace lwjgl

#endif // WEBOS_PLATFORM

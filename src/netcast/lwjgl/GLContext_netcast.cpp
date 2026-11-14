#ifdef NETCAST_PLATFORM

#include "lwjgl/GLContext.h"
#include "platform/RenderAPI.h"

#include <string>

namespace lwjgl
{
namespace GLContext
{
namespace detail
{
static GLCapabilities &netcastCaps()
{
	static GLCapabilities caps;
	return caps;
}
} // namespace detail

void setRequestedSamples(int) {}
int getRequestedSamples() { return 0; }

void instantiate()
{
	// The EGL context was made current in netcast::initialize(). Record the
	// GL ES extension list so capability checks keyed off it work.
	const unsigned char *ext = renderGetString(RenderStringQuery::Extensions);
	if (ext == nullptr)
		return;
	std::string cap;
	for (const char *p = reinterpret_cast<const char *>(ext);; ++p)
	{
		if (*p == ' ' || *p == '\0')
		{
			if (!cap.empty())
				detail::netcastCaps().add(cap);
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
	return detail::netcastCaps();
}

} // namespace GLContext
} // namespace lwjgl

#endif // NETCAST_PLATFORM

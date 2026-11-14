// main_webos.cpp -- entry point for LG webOS.
//
// webos::initialize() (src/webos/WebOSPlatform.cpp) brings up SDL2 and the
// OpenGL ES 1.x context; the rest mirrors src/netcast/main_netcast.cpp and,
// before it, src/pc/Main.cpp.
#ifdef WEBOS_PLATFORM

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include "client/Minecraft.h"
#include "net/minecraft/src/GameResources.h"

#include "lwjgl/GLContext.h"
#include "pc/render/PcRenderBackend.h"
#include "webos/WebOSPlatform.h"

namespace
{
std::string trimUsername(std::string value)
{
	if (value.size() >= 3 &&
		static_cast<unsigned char>(value[0]) == 0xef &&
		static_cast<unsigned char>(value[1]) == 0xbb &&
		static_cast<unsigned char>(value[2]) == 0xbf)
	{
		value.erase(0, 3);
	}
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return "";
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

// username.txt lives next to the executable when the install directory is
// writable and in $HOME otherwise (a webOS app bundle normally is not).
std::filesystem::path usernamePath()
{
	const std::filesystem::path beside = std::filesystem::path(GameResources::getExeDir()) / "username.txt";
	std::error_code ec;
	if (std::filesystem::exists(beside, ec))
		return beside;

	std::ofstream probe(beside, std::ios::app);
	if (probe.good())
	{
		probe.close();
		return beside;
	}

	const char *home = std::getenv("HOME");
	if (home != nullptr && home[0] != 0)
		return std::filesystem::path(home) / ".opticraft-username.txt";
	return beside;
}

std::string loadUsername()
{
	const std::filesystem::path path = usernamePath();
	std::ifstream input(path);
	std::string username;
	if (input && std::getline(input, username))
		username = trimUsername(username);
	if (!username.empty())
		return username;

	username = "Player";
	std::ofstream output(path, std::ios::trunc);
	if (output)
		output << username << '\n';
	return username;
}
} // namespace

int main(int argc, char *argv[])
{
	if (!webos::initialize(argc, argv))
	{
		webos::shutdown();
		return 1;
	}

	int result = 0;
	try
	{
		pcRenderBackendSetRequested(PcRenderBackendType::OpenGL);
		lwjgl::GLContext::instantiate();

		jstring username = loadUsername();
		jstring auth = "-";
		Minecraft::start(&username, &auth);
	}
	catch (const std::exception &e)
	{
		webos::log("fatal: %s", e.what());
		result = 1;
	}
	catch (...)
	{
		webos::log("fatal: unknown exception");
		result = 1;
	}

	webos::shutdown();
	return result;
}

#endif // WEBOS_PLATFORM

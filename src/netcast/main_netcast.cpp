// main_netcast.cpp -- entry point for LG NetCast 4.x.
//
// The TV's app manager starts the program with "--service <name> --auid <hex>";
// those arguments must reach LGNC_SYSTEM_Initialize() unchanged (the launcher
// script passes "$@" through). Everything else mirrors src/pc/Main.cpp.
#ifdef NETCAST_PLATFORM

#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include "client/Minecraft.h"
#include "net/minecraft/src/GameResources.h"
#include "net/minecraft/src/Tessellator.h"

#include "lwjgl/GLContext.h"
#include "netcast/NetcastPlatform.h"
#include "pc/render/PcRenderBackend.h"

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

std::string loadUsername()
{
	const std::filesystem::path path = std::filesystem::path(GameResources::getExeDir()) / "username.txt";
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
	if (!netcast::initialize(argc, argv))
	{
		netcast::shutdown();
		return 1;
	}

	if (netcast::configInt("bench", 0) != 0)
	{
		netcast::runBench();
		netcast::shutdown();
		return 0;
	}

	if (netcast::configInt("autobench", 1) != 0)
		netcast::calibrate(); // measures this TV, picks gamepad/size, resets GL (autobench=0 skips it)
	else if (netcast::configInt("gamepad", 1) != 0)
		netcast::enableGamepad();

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
		netcast::log("fatal: %s", e.what());
		result = 1;
	}
	catch (...)
	{
		netcast::log("fatal: unknown exception");
		result = 1;
	}

	netcast::shutdown();
	return result;
}

#endif // NETCAST_PLATFORM

// ClientPlatformPolicy_WEBOS.cpp -- webOS policy for the shared client startup.
//
// The differences from the desktop profile are all about the app sandbox: a
// webOS application bundle is installed read-only, so the .minecraft directory
// (options.txt, saves/, screenshots/) has to live somewhere writable. The order
// is an explicit override, then $HOME, then the install folder as a last resort
// (which only works when the app was started from a writable location).
#include "platform/ClientPlatformPolicy.h"

#include "net/minecraft/src/GameResources.h"
#include "net/minecraft/src/GameSettings.h"
#include "pc/CrashHandler.h"
#include "platform/PlatformConfig.h"
#include "webos/WebOSPlatform.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
bool isWritableDirectory(const std::filesystem::path &dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec))
        return false;
    const std::filesystem::path probe = dir / ".write-probe";
    {
        std::ofstream out(probe, std::ios::app);
        if (!out.good())
            return false;
    }
    std::filesystem::remove(probe, ec);
    return true;
}
} // namespace

namespace ClientPlatformPolicy
{
int initialWidth()
{
    // The window already fills the panel; start the GUI at the drawable size so
    // the first setDisplayMode() call does not shrink it.
    return webos::width() > 0 ? webos::width() : 1920;
}

int initialHeight()
{
    return webos::height() > 0 ? webos::height() : 1080;
}

std::string minecraftDirectory()
{
    const std::filesystem::path exeDir = GameResources::getExeDir();

    if (const char *override = std::getenv("OPTICRAFT_DATA_DIR"))
    {
        const std::filesystem::path dir(override);
        if (!dir.empty() && isWritableDirectory(dir))
            return dir.string();
    }

    if (const char *home = std::getenv("HOME"))
    {
        if (home[0] != 0)
        {
            const std::filesystem::path dir = std::filesystem::path(home) / ".opticraft";
            if (isWritableDirectory(dir))
                return dir.string();
        }
    }

    // /tmp is writable in the app sandbox and survives while the app runs; saves
    // kept there are lost when the TV reboots, but it is better than not saving.
    if (isWritableDirectory(std::filesystem::path("/tmp/opticraft")))
        return "/tmp/opticraft";

    return (exeDir / ".minecraft").string();
}

bool saveConverterUsesSavesSubdirectory()
{
    return true;
}

void applyGameSettingsDefaults(GameSettings *settings)
{
    if (settings == nullptr)
        return;

    // webos.cfg overrides, applied after options.txt has been read so a TV can
    // be tuned without editing the world/options file. -1 (the default) keeps
    // whatever the player chose in the game's own settings.
    const int renderDistance = webos::configInt("render_distance", -1);
    if (renderDistance >= 0)
        settings->renderDistance = renderDistance;
    const int renderDistanceFine = webos::configInt("render_distance_fine", -1);
    if (renderDistanceFine > 0)
        settings->ofRenderDistanceFine = renderDistanceFine;

    // Connected textures split every chunk section into one mesh (and one draw
    // call) per connected texture, which is exactly the wrong trade on a TV:
    // measured on webOS, turning them off removes most of the per-frame draws.
    // 0 = off (default), 1 = fast, 2 = fancy.
    const int connected = webos::configInt("connected_textures", 0);
    settings->ofConnectedTextures = connected <= 0 ? 3 : (connected == 1 ? 1 : 2);
    settings->ofNaturalTextures = false;
    settings->ofCustomColors = false;

    // One chunk rebuild per frame, never the OptiFine "dynamic" x3: a rebuild is
    // 20-50 ms on this CPU and stacking three of them per frame is what turned
    // walking into a slideshow.
    settings->ofChunkUpdates = 1;
    settings->ofChunkUpdatesDynamic = false;

    // Log what actually reached the game: options.txt, the platform defaults and
    // these overrides all write the same fields, so the log is the only way to
    // tell from a bug report which values are in force.
    webos::log("game settings: connectedTextures=%d naturalTextures=%d chunkUpdates=%d dynamic=%d "
               "renderDistance=%d fineDistance=%d",
               static_cast<int>(settings->ofConnectedTextures),
               settings->ofNaturalTextures ? 1 : 0,
               static_cast<int>(settings->ofChunkUpdates),
               settings->ofChunkUpdatesDynamic ? 1 : 0,
               static_cast<int>(settings->renderDistance),
               static_cast<int>(settings->ofRenderDistanceFine));

#if PLATFORM_PC_LEGACY
    settings->particleSetting = 2;
    settings->ofVoidParticles = false;
    settings->ofWaterParticles = false;
    settings->ofRainSplash = false;
    settings->ofPortalParticles = false;
    settings->ofDrippingWaterLava = false;
    settings->ofWeather = false;
    settings->ofSky = false;
    settings->ofSunMoon = false;
    settings->ofClouds = 3;
#endif
}

void preloadStartupTextures(RenderEngine *)
{
}

void releaseWorldEntryAssets(RenderEngine *)
{
}

int panoramaSampleGrid()
{
    // Without the framebuffer copy the 256x256 pre-pass is wasted work: skip it.
    return webos::configInt("copy_fb", 0) != 0 ? 2 : 1;
}

void reportCrash(const std::string &description)
{
    CrashHandler::Crash(description);
}
} // namespace ClientPlatformPolicy

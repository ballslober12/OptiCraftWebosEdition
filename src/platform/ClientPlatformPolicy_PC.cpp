#include "platform/ClientPlatformPolicy.h"
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
#include "platform/TvPlatform.h"
#endif

#include "net/minecraft/src/GameResources.h"
#include "net/minecraft/src/GameSettings.h"
#include "pc/CrashHandler.h"
#include "platform/PlatformConfig.h"

namespace ClientPlatformPolicy
{
int initialWidth()
{
    return 854;
}

int initialHeight()
{
    return 480;
}

std::string minecraftDirectory()
{
    return GameResources::getExeDir() + "/.minecraft";
}

bool saveConverterUsesSavesSubdirectory()
{
    return true;
}

void applyGameSettingsDefaults(GameSettings* settings)
{
#if PLATFORM_PC_LEGACY
    if (settings == nullptr)
        return;

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
#else
    (void)settings;
#endif
}

void preloadStartupTextures(RenderEngine*)
{
}

void releaseWorldEntryAssets(RenderEngine*)
{
}

int panoramaSampleGrid()
{
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
    // 384 blended draws per frame is far too much for a tile-based SGX; the PS2
    // build's 2x2 accumulation looks nearly the same on a 256x256 target.
    // without the render-to-texture copy the panorama is drawn full-screen, so keep
    // the number of full-screen blended layers minimal
    return tvc::configInt("copy_fb", 0) != 0 ? 2 : 1;
#else
    return 8;
#endif
}


void reportCrash(const std::string& description)
{
    CrashHandler::Crash(description);
}
}

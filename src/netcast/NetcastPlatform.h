// NetcastPlatform.h -- LG NetCast (lgncopenapi + EGL/GLES1) platform glue.
//
// Everything the game needs from the TV that is not rendering: LGNC bring-up,
// the EGL window surface, the application message loop and the translation of
// remote / keyboard / mouse / gamepad events into the lwjgl::Keyboard and
// lwjgl::Mouse queues the shared game code already consumes.
//
// LGNC delivers its callbacks on a thread of its own. They only append to a
// mutex-protected queue; pumpEvents() (main thread, called from
// lwjgl::Display::processMessages) is the only place that touches the lwjgl
// queues, which are deliberately not thread-safe.
#pragma once

#include <string>

namespace netcast
{

// LGNC_SYSTEM_Initialize + resolution + EGL window surface + GLES1 context.
// Returns false (and logs why) if any step fails.
bool initialize(int argc, char **argv);
void shutdown();

// eglSwapBuffers on the window surface.
void swapBuffers();

// Set the alpha of the framebuffer to 1 (colour and GL state untouched). The TV
// composites the window with its per-pixel alpha (all EGL configs are ARGB8888);
// Minecraft clears with alpha 0, which would make the world transparent. Same
// fix as the SDL nc4 video driver. Called by swapBuffers() unless opaque=0.
void makeOpaque();

// Drain the LGNC callback queue and poll held-key/axis state into lwjgl.
void pumpEvents();

int  width();
int  height();
bool closeRequested();
void requestClose();
bool hasFocus();

// Directory that contains the executable (with trailing '/'); assets/ and
// resources/ live beside it, exactly like the desktop layout.
const std::string &exeDir();

// Optional <exeDir>/netcast.cfg ("key=value" lines, integers). See
// deploy/netcast/netcast.cfg for every key. Missing file = defaults.
int configInt(const char *key, int fallback);

// Append a line to the log (stderr + <exeDir>/netcast.log). The TV's app
// framework swallows printf/puts, so everything goes through write(2).
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Recreate the EGL window surface at another size (LGNC_SYSTEM_SetResolution +
// new surface on the same context). Returns false if the TV refused; the previous
// size is restored in that case.
bool resizeSurface(int w, int h);

// The gamepad API is registered separately from initialize() so the self-test can
// measure what it costs (see calibrate()).
void enableGamepad();
void disableGamepad();

// Startup calibration (NetcastBench.cpp): times the display path, decides about the
// gamepad API and the render size, and restores a clean GL state.
void calibrate();

// "bench=1" in netcast.cfg: time the suspects one by one, log, then exit (NetcastBench.cpp).
void runBench(bool quick = false);

// ---- profiling (logged every 5 s by swapBuffers) ---------------------------
extern unsigned g_statDraws;     // glDraw* calls since the last report
extern double   g_statCopyMs;    // time blocked in framebuffer->texture copies
extern double   g_statAudioCpu;
extern unsigned g_statUploads;   // glTex(Sub)Image2D calls
extern double   g_statUploadKB;  // ... and bytes uploaded
extern double   g_statUploadMs;  // time spent in them  // CPU seconds used by the audio thread (written by it)

// ---- controller ------------------------------------------------------------
// Analog state for platformGamepadSnapshot(): sticks in -1..1 after the radial
// deadzone, +Y = stick pushed down. Returns false while no pad is connected.
bool padSnapshot(float &leftX, float &leftY, float &rightX, float &rightY);

} // namespace netcast

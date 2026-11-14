// WebOSPlatform.h -- LG webOS (SDL2 + EGL/OpenGL ES 1.x) platform glue.
//
// Everything the game needs from the TV that is not rendering: the SDL2 window
// and the OpenGL ES 1.x context, the event loop, and the translation of remote /
// keyboard / mouse / gamepad input into the lwjgl::Keyboard and lwjgl::Mouse
// queues the shared game code consumes.
//
// The window, the GL context and all input come from SDL2, using the webosbrew
// SDL-webOS build (Wayland + xkbcommon backends) that the TV compositor accepts.
// Input is polled on the main thread only (SDL_PollEvent from
// lwjgl::Display::processMessages), so the lwjgl queues are never touched from a
// second thread.
#pragma once

#include <string>

namespace webos
{

// SDL video/gamecontroller init, window creation, GLES1 context, input pump.
// Returns false (and logs why) if any step fails.
bool initialize(int argc, char **argv);
void shutdown();

// SDL_GL_SwapWindow on the TV window, plus the per-5s frame statistics.
void swapBuffers();

// Set the alpha of the framebuffer to 1 (colour and GL state untouched). The
// webOS compositor treats alpha < 1 as transparent and the game clears with
// alpha 0, so this is called by swapBuffers() unless opaque=0 in webos.cfg.
void makeOpaque();

// Drain SDL events and poll held-key/stick state into lwjgl.
void pumpEvents();

int  width();
int  height();
bool closeRequested();
void requestClose();
bool hasFocus();

// Directory that contains the executable (with trailing '/'); assets/ and
// resources/ live beside it, exactly like the desktop layout.
const std::string &exeDir();

// Optional <exeDir>/webos.cfg ("key=value" lines, integers). See
// deploy/webos/webos.cfg for every key. Missing file = defaults.
int configInt(const char *key, int fallback);

// Append a line to the log (stderr + <exeDir>/webos.log). The TV's app
// framework swallows printf/puts, so the log is written with write(2).
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Recreate the GL surface at another size through SDL_SetWindowSize. Returns
// false if SDL refused; the previous size is kept in that case.
bool resizeSurface(int w, int h);

// ---- profiling (logged every 5 s by swapBuffers) ---------------------------
extern unsigned g_statDraws;     // glDraw* calls since the last report
extern double   g_statCopyMs;    // time blocked in framebuffer->texture copies
extern double   g_statAudioCpu;
extern unsigned g_statUploads;   // glTex(Sub)Image2D calls
extern double   g_statUploadKB;  // ... and bytes uploaded
extern double   g_statUploadMs;  // time spent in them

// ---- controller ------------------------------------------------------------
// Analog state for platformGamepadSnapshot(): sticks in -1..1 after the radial
// deadzone, +Y = stick pushed down. Returns false while no pad is connected.
bool padSnapshot(float &leftX, float &leftY, float &rightX, float &rightY);

} // namespace webos

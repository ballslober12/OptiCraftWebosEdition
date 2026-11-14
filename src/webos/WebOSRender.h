// WebOSRender.h -- render-scale control for the webOS GLES1 renderer.
//
// The TV panel is 1080p and the game is fill-rate bound on its GPU, so the frame
// is rendered into an offscreen colour texture at a fraction of the panel size
// and stretched to the window in one textured quad. That is the single biggest
// frame-time lever on a tile-based GPU: at 67% scale the GPU shades 55% of the
// pixels, at 50% it shades 25%.
//
// Implemented in src/webos/render/WebOSGlesRenderBackend.cpp (it owns the GL
// state caches that the blit has to invalidate).
#pragma once

namespace webos
{
namespace render
{

// Creates the offscreen target and binds it (or binds the default framebuffer
// when scaling is off). Call once with the GL ES 1.x context current.
bool scaleInit();
void scaleShutdown();

// True when the driver offers GL_OES_framebuffer_object and scaling is allowed.
bool scaleAvailable();

// Blits the finished frame to the window. Returns true when the blit happened,
// in which case the caller must not touch the default framebuffer (the blit
// already writes alpha 1, so makeOpaque() is unnecessary).
bool scalePresent(int nativeWidth, int nativeHeight);

// 0 = automatic (adapt to target_fps), 1 = native, 2 = 80%, 3 = 67%, 4 = 50%,
// 10..100 = explicit percent. Values it cannot use fall back to automatic.
void scaleSetMode(int mode);
int  scaleMode();
float scaleValue();

// True while frames are rendered offscreen. Readback (screenshots) has to be
// expressed in the offscreen size, which scaleFramebufferSize() returns.
bool scaleActive();
void scaleFramebufferSize(int &width, int &height);

// The window/drawable changed size (Display::setDisplayMode or a compositor
// resize). Rebuilds the offscreen target at the new size.
void scaleResize(int nativeWidth, int nativeHeight);

// Frame time in milliseconds, for the automatic mode (once per frame).
void scaleNotifyFrameTime(double ms);

// The game-visible viewport (unscaled). The renderer routes renderViewport and
// renderGetViewport through these so the game keeps working in panel pixels.
void scaleViewport(int x, int y, int width, int height);
void scaleGetViewport(int *values);

} // namespace render
} // namespace webos

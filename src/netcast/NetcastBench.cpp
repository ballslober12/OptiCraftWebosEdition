// NetcastBench.cpp -- "bench=1" in netcast.cfg: time the suspects one by one.
//
// Runs on the real EGL surface right after initialisation, writes one line per
// test to netcast.log, then the program exits. Each test reports the average and
// worst frame time including eglSwapBuffers, so it separates "the display is
// pacing us" (clear-only tests) from "the GPU is slow at X" (the others).
#ifdef NETCAST_PLATFORM

#include "netcast/NetcastPlatform.h"

#include <cstring>
#include <time.h>
#include <vector>

#include <EGL/egl.h>
#include <GLES/gl.h>

namespace netcast
{
namespace
{

double nowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec * 1e-6;
}

struct Quad { float x, y, w, h; };

GLuint g_tex = 0;
GLuint g_vbo = 0;
std::vector<float> g_small; // 250 quads as 6 verts of (x y u v)
const int kSmall = 250;

void fullQuad(float alpha)
{
	static const float v[] = { -1,-1,0,0,  1,-1,1,0,  -1,1,0,1,  1,1,1,1 };
	glColor4f(1, 1, 1, alpha);
	glVertexPointer(2, GL_FLOAT, 16, v);
	glTexCoordPointer(2, GL_FLOAT, 16, v + 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

typedef void (*Scene)();

void sceneClearColor()
{
	glClearColor(0.2f, 0.4f, 0.8f, 0.0f); // alpha 0, like the game
	glClear(GL_COLOR_BUFFER_BIT);
}

void sceneClearAll()
{
	glClearColor(0.2f, 0.4f, 0.8f, 0.0f); // alpha 0, like the game
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void sceneBlend8()
{
	sceneClearAll();
	glEnable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	for (int i = 0; i < 8; ++i)
		fullQuad(0.5f);
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
}

void sceneOpaque8()
{
	sceneClearAll();
	glEnable(GL_TEXTURE_2D);
	for (int i = 0; i < 8; ++i)
		fullQuad(1.0f);
	glDisable(GL_TEXTURE_2D);
}

void sceneSmallClient()
{
	sceneClearAll();
	glEnable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
	for (int i = 0; i < kSmall; ++i)
	{
		const float *q = &g_small[static_cast<size_t>(i) * 24];
		glVertexPointer(2, GL_FLOAT, 16, q);
		glTexCoordPointer(2, GL_FLOAT, 16, q + 2);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
}

void sceneSmallVbo()
{
	sceneClearAll();
	glEnable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	for (int i = 0; i < kSmall; ++i)
	{
		const size_t off = static_cast<size_t>(i) * 24 * sizeof(float);
		glVertexPointer(2, GL_FLOAT, 16, reinterpret_cast<const void *>(off));
		glTexCoordPointer(2, GL_FLOAT, 16, reinterpret_cast<const void *>(off + 8));
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
}

void sceneSubUpload()
{
	static std::vector<unsigned char> px(16 * 16 * 4, 200);
	glEnable(GL_TEXTURE_2D);
	for (int i = 0; i < 10; ++i)
		glTexSubImage2D(GL_TEXTURE_2D, 0, i * 16, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	sceneOpaque8();
}

void sceneFullUpload()
{
	static std::vector<unsigned char> px(256 * 256 * 4, 100);
	glEnable(GL_TEXTURE_2D);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	sceneOpaque8();
}

void sceneCopyFb()
{
	sceneOpaque8();
	glEnable(GL_TEXTURE_2D);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 256, 256);
	glDisable(GL_TEXTURE_2D);
}

double runTest(const char *name, int interval, Scene scene, bool opaque = true, double seconds = 2.5)
{
	EGLDisplay dpy = eglGetCurrentDisplay();
	eglSwapInterval(dpy, interval);
	for (int i = 0; i < 8; ++i) { scene(); if (opaque) makeOpaque(); eglSwapBuffers(dpy, eglGetCurrentSurface(EGL_DRAW)); } // warm-up
	const double start = nowMs();
	double worst = 0.0, last = start;
	int frames = 0;
	while (nowMs() - start < seconds * 1000.0)
	{
		scene();
		if (opaque)
			makeOpaque();
		eglSwapBuffers(dpy, eglGetCurrentSurface(EGL_DRAW));
		const double t = nowMs();
		if (t - last > worst) worst = t - last;
		last = t;
		++frames;
	}
	const double avg = (nowMs() - start) / frames;
	log("bench %-38s interval %d opaque %d : %7.2f ms/frame (%5.1f fps), worst %6.1f ms", name, interval, opaque ? 1 : 0, avg, 1000.0 / avg, worst);
	return avg;
}

void benchSetup()
{
	glViewport(0, 0, width(), height());
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(-1, 1, -1, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	std::vector<unsigned char> px(256 * 256 * 4);
	for (size_t i = 0; i < px.size(); ++i)
		px[i] = static_cast<unsigned char>((i * 7) & 0xff);
	glGenTextures(1, &g_tex);
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	g_small.resize(static_cast<size_t>(kSmall) * 24);
	for (int i = 0; i < kSmall; ++i)
	{
		const float x = -0.95f + 1.9f * ((i * 37) % 100) / 100.0f, y = -0.95f + 1.9f * ((i * 53) % 100) / 100.0f, s = 0.04f;
		const float quad[6][4] = { {x,y,0,0}, {x+s,y,1,0}, {x,y+s,0,1}, {x+s,y,1,0}, {x+s,y+s,1,1}, {x,y+s,0,1} };
		std::memcpy(&g_small[static_cast<size_t>(i) * 24], quad, sizeof(quad));
	}
	glGenBuffers(1, &g_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(g_small.size() * sizeof(float)), g_small.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

}

// Put GL back to its power-on state. The tests above enable client arrays, change
// the projection and blend function and create objects; the game assumes none of
// that is there (and its state cache starts from the GL defaults).
void resetGlState()
{
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	if (g_vbo) glDeleteBuffers(1, &g_vbo);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (g_tex) glDeleteTextures(1, &g_tex);
	g_vbo = g_tex = 0;
	for (int unit = 1; unit >= 0; --unit)
	{
		glClientActiveTexture(GL_TEXTURE0 + unit);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glActiveTexture(GL_TEXTURE0 + unit);
		glDisable(GL_TEXTURE_2D);
		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
	}
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	const GLenum caps[] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_ALPHA_TEST, GL_FOG, GL_LIGHTING, GL_LIGHT0, GL_LIGHT1,
	                        GL_COLOR_MATERIAL, GL_NORMALIZE, GL_RESCALE_NORMAL, GL_POLYGON_OFFSET_FILL, GL_SCISSOR_TEST };
	for (GLenum c : caps)
		glDisable(c);
	glBlendFunc(GL_ONE, GL_ZERO);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glColor4f(1, 1, 1, 1);
	glClearColor(0, 0, 0, 0);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glViewport(0, 0, width(), height());
}

} // namespace

void runBench(bool quick)
{
	log("=== %s: %d x %d ===", quick ? "self-test" : "bench", width(), height());
	benchSetup();

	if (quick)
	{
		// ~9 s: the few measurements that tell display pacing from GPU work.
		runTest("clear colour only (alpha 0)", 1, sceneClearColor, false, 1.2);
		runTest("clear colour only (alpha made 1)", 1, sceneClearColor, true, 1.2);
		runTest("clear colour only (alpha made 1)", 0, sceneClearColor, true, 1.2);
		runTest("8 blended full-screen quads", 0, sceneBlend8, true, 1.2);
		runTest("250 small draws, client arrays", 0, sceneSmallClient, true, 1.2);
		runTest("10x glTexSubImage2D 16x16 + draw", 0, sceneSubUpload, true, 1.2);
		runTest("glTexImage2D 256x256 + draw", 0, sceneFullUpload, true, 1.2);
		resetGlState();
		log("=== self-test done ===");
		return;
	}

	// 1) what is the display pacing us to?
	// alpha 0 as Minecraft clears it, with and without the alpha fix
	runTest("clear colour only (alpha 0)", 1, sceneClearColor, false);
	runTest("clear colour only (alpha made 1)", 1, sceneClearColor, true);
	runTest("clear colour only (alpha made 1)", 2, sceneClearColor, true);
	runTest("clear colour only (alpha made 1)", 0, sceneClearColor, true);
	// 2) GPU work, unthrottled
	runTest("clear colour+depth+stencil", 0, sceneClearAll);
	runTest("8 opaque full-screen textured quads", 0, sceneOpaque8);
	runTest("8 blended full-screen textured quads", 0, sceneBlend8);
	runTest("250 small draws, client arrays", 0, sceneSmallClient);
	runTest("250 small draws, vertex buffer", 0, sceneSmallVbo);
	runTest("10x glTexSubImage2D 16x16 + draw", 0, sceneSubUpload);
	runTest("glTexImage2D 256x256 + draw", 0, sceneFullUpload);
	runTest("glCopyTexSubImage2D 256x256 + draw", 0, sceneCopyFb);
	// 3) the setting the game uses
	runTest("8 blended quads, alpha left as drawn", 1, sceneBlend8, false);
	runTest("8 blended quads, alpha made 1", 1, sceneBlend8, true);
	resetGlState();
	log("=== bench done ===");
}


// ---------------------------------------------------------------------------
// Startup calibration. Everything here is measured on the TV it runs on:
//   1. baseline cost of presenting an (almost) empty frame;
//   2. the same with the gamepad API registered -> keep it only if it is free;
//   3. the same at 960x540 and 640x360 -> use a smaller surface only if presenting
//      gets clearly cheaper;
// then GL is put back to a clean state and the game starts.
void calibrate()
{
	log("=== calibration: starting at %d x %d ===", width(), height());
	benchSetup();

	const double base = runTest("baseline, no gamepad API", 0, sceneClearColor, true, 1.0);
	bool padOn = false;
	if (configInt("gamepad", 1) != 0)
	{
		enableGamepad();
		const double withPad = runTest("baseline, gamepad API on", 0, sceneClearColor, true, 1.0);
		if (withPad > base * 1.4 + 8.0)
		{
			log("gamepad API makes presenting slower (%.0f -> %.0f ms): not using it. Set gamepad=1 AND force_gamepad=1 to override.", base, withPad);
			if (configInt("force_gamepad", 0) == 0)
				disableGamepad();
			else
				padOn = true;
		}
		else
		{
			padOn = true;
		}
	}
	log("gamepad API: %s", padOn ? "on" : "off");
	runTest("vsync on (interval 1)", 1, sceneClearColor, true, 1.0);

	int bestW = width(), bestH = height();
	if (configInt("autosize", 1) != 0)
	{
		struct Size { int w, h; double ms; };
		Size sizes[3] = { { width(), height(), base }, { 960, 540, 1e9 }, { 640, 360, 1e9 } };
		for (int i = 1; i < 3; ++i)
		{
			if (!resizeSurface(sizes[i].w, sizes[i].h))
			{
				log("size %dx%d: TV refused", sizes[i].w, sizes[i].h);
				continue;
			}
			glViewport(0, 0, width(), height());
			sizes[i].ms = runTest("baseline at this size", 0, sceneClearColor, true, 1.0);
		}
		// Use a smaller surface only when it clearly helps.
		int pick = 0;
		for (int i = 1; i < 3; ++i)
			if (sizes[i].ms < sizes[pick].ms * 0.7)
				pick = i;
		bestW = sizes[pick].w;
		bestH = sizes[pick].h;
		log("size choice: %dx%d (%.0f ms vs %.0f ms at %dx%d)", bestW, bestH, sizes[pick].ms, base, sizes[0].w, sizes[0].h);
		if (width() != bestW || height() != bestH)
			resizeSurface(bestW, bestH);
	}

	glViewport(0, 0, width(), height());
	runTest("8 blended full-screen quads", 0, sceneBlend8, true, 1.0);
	runTest("250 small draws, client arrays", 0, sceneSmallClient, true, 1.0);

	resetGlState();
	log("=== calibration done: %d x %d, gamepad %s ===", width(), height(), padOn ? "on" : "off");
}

} // namespace netcast

#endif // NETCAST_PLATFORM

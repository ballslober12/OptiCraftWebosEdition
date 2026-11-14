// NetcastGlesRenderBackend.cpp -- OpenGL ES 1.x implementation of the desktop
// render backend interface (PC_RENDER_BACKEND_FUNCTIONS).
//
// The shared game code takes the desktop path on NetCast: retained-mode display
// lists for terrain/models/sky/HUD, quad primitives, and a handful of desktop-GL
// state calls. OpenGL ES 1.x has none of display lists, GL_QUADS, glColorMaterial,
// GL_CLAMP, glClearDepth/glOrtho/glFrustum (double), occlusion queries, or
// GL_TEXTURE_MAX_LEVEL. This file bridges exactly those gaps and nothing else:
//
//   display lists   recorded as a small command list. Draws inside a list are
//                   copied into a GL buffer object (ES 1.1) once, then replayed
//                   by binding it, which is far cheaper than resubmitting
//                   client arrays every frame on this class of GPU.
//   GL_QUADS        expanded to triangles through a shared 16-bit index buffer,
//                   in batches that fit 16-bit indices.
//   occlusion       reported unsupported, so the game never issues queries.
//   mipmaps/aniso   reported unsupported (ES1 needs complete chains, and the
//                   game uploads only the levels it selected).
//   MSAA            reported unsupported (EGL config is chosen in
//                   NetcastPlatform.cpp; the option stays hidden).
#ifdef NETCAST_PLATFORM

#include "platform/RenderAPI.h"
#include "pc/render/PcRenderBackendApi.h"
#include "netcast/NetcastPlatform.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <unordered_map>
#include <vector>

#include <GLES/gl.h>
#include <GLES/glext.h>

namespace
{

constexpr int kQuadBatchVertices = 16384; // 4096 quads -> 24576 indices, fits uint16
constexpr int kMaxListDepth = 8;

// ------------------------------------------------------------ immediate ----
GLenum toGlFactor(RenderBlendFactor f)
{
	switch (f)
	{
		case RenderBlendFactor::Zero: return GL_ZERO;
		case RenderBlendFactor::One: return GL_ONE;
		case RenderBlendFactor::SrcColor: return GL_SRC_COLOR;
		case RenderBlendFactor::OneMinusSrcColor: return GL_ONE_MINUS_SRC_COLOR;
		case RenderBlendFactor::SrcAlpha: return GL_SRC_ALPHA;
		case RenderBlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
		case RenderBlendFactor::DstAlpha: return GL_DST_ALPHA;
		case RenderBlendFactor::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
		case RenderBlendFactor::DstColor: return GL_DST_COLOR;
		case RenderBlendFactor::OneMinusDstColor: return GL_ONE_MINUS_DST_COLOR;
	}
	return GL_ONE;
}

GLenum toGlCompare(RenderCompare c)
{
	switch (c)
	{
		case RenderCompare::Never: return GL_NEVER;
		case RenderCompare::Less: return GL_LESS;
		case RenderCompare::Equal: return GL_EQUAL;
		case RenderCompare::LessEqual: return GL_LEQUAL;
		case RenderCompare::Greater: return GL_GREATER;
		case RenderCompare::NotEqual: return GL_NOTEQUAL;
		case RenderCompare::GreaterEqual: return GL_GEQUAL;
		case RenderCompare::Always: return GL_ALWAYS;
	}
	return GL_ALWAYS;
}

GLenum toGlCap(RenderCapability c)
{
	switch (c)
	{
		case RenderCapability::Texture2D: return GL_TEXTURE_2D;
		case RenderCapability::ColorMaterial: return GL_COLOR_MATERIAL;
		case RenderCapability::CullFace: return GL_CULL_FACE;
		case RenderCapability::AlphaTest: return GL_ALPHA_TEST;
		case RenderCapability::Blend: return GL_BLEND;
		case RenderCapability::DepthTest: return GL_DEPTH_TEST;
		case RenderCapability::Fog: return GL_FOG;
		case RenderCapability::Lighting: return GL_LIGHTING;
		case RenderCapability::Normalize: return GL_NORMALIZE;
		case RenderCapability::RescaleNormal: return GL_RESCALE_NORMAL;
		case RenderCapability::Light0: return GL_LIGHT0;
		case RenderCapability::Light1: return GL_LIGHT1;
		case RenderCapability::PolygonOffsetFill: return GL_POLYGON_OFFSET_FILL;
	}
	return GL_TEXTURE_2D;
}

GLenum toGlFace(RenderFace f)
{
	return f == RenderFace::Front ? GL_FRONT : f == RenderFace::Back ? GL_BACK : GL_FRONT_AND_BACK;
}

GLenum toGlMatrixMode(RenderMatrixMode m)
{
	return m == RenderMatrixMode::Projection ? GL_PROJECTION : m == RenderMatrixMode::Texture ? GL_TEXTURE : GL_MODELVIEW;
}

// -------------------------------------------------------------- indices ----
GLuint g_quadIbo = 0;
bool g_quadIboTried = false;
std::vector<GLushort> g_quadIdx; // client copy, also the IBO source

void ensureQuadIndices()
{
	if (!g_quadIdx.empty())
		return;
	g_quadIdx.resize(static_cast<size_t>(kQuadBatchVertices / 4) * 6);
	for (int q = 0; q < kQuadBatchVertices / 4; ++q)
	{
		const GLushort v = static_cast<GLushort>(q * 4);
		GLushort *i = &g_quadIdx[static_cast<size_t>(q) * 6];
		i[0] = v;     i[1] = static_cast<GLushort>(v + 1); i[2] = static_cast<GLushort>(v + 2);
		i[3] = v;     i[4] = static_cast<GLushort>(v + 2); i[5] = static_cast<GLushort>(v + 3);
	}
}

bool g_useVbo = false;
bool g_vboDecided = false;

void decideVbo()
{
	if (g_vboDecided)
		return;
	g_vboDecided = true;
	const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
	// "OpenGL ES-CM 1.1" / "OpenGL ES 1.1 ...": buffer objects are core in 1.1.
	// Buffer objects are opt-in (use_vbo=1 in netcast.cfg): client arrays from the
	// retained copy are the conservative path. Needs GL ES 1.1.
	g_useVbo = netcast::configInt("use_vbo", 0) != 0 && version != nullptr && std::strstr(version, "1.1") != nullptr;
	netcast::log("display lists: %s", g_useVbo ? "vertex buffer objects" : "client arrays");
}

GLuint quadIbo()
{
	if (!g_useVbo)
		return 0;
	if (g_quadIbo == 0 && !g_quadIboTried)
	{
		g_quadIboTried = true;
		ensureQuadIndices();
		glGenBuffers(1, &g_quadIbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quadIbo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(g_quadIdx.size() * sizeof(GLushort)),
		             g_quadIdx.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	return g_quadIbo;
}

// ------------------------------------------------------------ state cache ----
// The SGX driver on this CPU spends more time validating redundant GL calls than
// the GPU spends drawing small UI meshes, so every state call that only repeats
// the current value is dropped here. Only this backend talks to GL, so the cache
// cannot be bypassed.
struct ClientArrays
{
	bool vertex = false, tex0 = false, tex1 = false, color = false, normal = false;
	GLenum unit = GL_TEXTURE0;
} g_cs;

inline void clientUnit(GLenum unit)
{
	if (g_cs.unit != unit)
	{
		glClientActiveTexture(unit);
		g_cs.unit = unit;
	}
}

inline void clientArray(bool &current, bool want, GLenum array)
{
	if (current == want)
		return;
	if (want)
		glEnableClientState(array);
	else
		glDisableClientState(array);
	current = want;
}

GLenum g_activeUnit = GL_TEXTURE0;
GLint g_boundTex[4] = {-1, -1, -1, -1};
signed char g_tex2dOn[4] = {-1, -1, -1, -1};
signed char g_capOn[16];
bool g_capInit = false;
bool g_colorValid = false;
float g_color[4] = {0, 0, 0, 0};

inline int unitIndex()
{
	const int i = static_cast<int>(g_activeUnit) - static_cast<int>(GL_TEXTURE0);
	return i < 0 ? 0 : (i > 3 ? 3 : i);
}

int capSlot(GLenum cap)
{
	switch (cap)
	{
		case GL_COLOR_MATERIAL: return 0;
		case GL_CULL_FACE: return 1;
		case GL_ALPHA_TEST: return 2;
		case GL_BLEND: return 3;
		case GL_DEPTH_TEST: return 4;
		case GL_FOG: return 5;
		case GL_LIGHTING: return 6;
		case GL_NORMALIZE: return 7;
		case GL_RESCALE_NORMAL: return 8;
		case GL_LIGHT0: return 9;
		case GL_LIGHT1: return 10;
		case GL_POLYGON_OFFSET_FILL: return 11;
		default: return -1;
	}
}

void execEnable(GLenum cap, bool on)
{
	if (!g_capInit)
	{
		std::memset(g_capOn, -1, sizeof(g_capOn));
		g_capInit = true;
	}
	signed char *slot = nullptr;
	if (cap == GL_TEXTURE_2D)
		slot = &g_tex2dOn[unitIndex()];
	else
	{
		const int i = capSlot(cap);
		if (i >= 0)
			slot = &g_capOn[i];
	}
	if (slot != nullptr)
	{
		if (*slot == (on ? 1 : 0))
			return;
		*slot = on ? 1 : 0;
	}
	if (on)
		glEnable(cap);
	else
		glDisable(cap);
}

void execActiveTexture(GLenum unit)
{
	if (g_activeUnit == unit)
		return;
	glActiveTexture(unit);
	g_activeUnit = unit;
}

void execBind(GLint tex)
{
	GLint &cur = g_boundTex[unitIndex()];
	if (cur == tex)
		return;
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex));
	cur = tex;
}

// Texture completeness guard. ES 1.x treats a texture whose MIN filter needs
// mipmaps (the default) but has none as incomplete and samples it as plain
// white -- which is what unfiltered/late-parameterised textures looked like.
// Track which texture ids have had their filter set and default the rest.
std::vector<unsigned char> g_texHasFilter;

bool texGuardOn()
{
	static const bool on = netcast::configInt("tex_guard", 0) != 0;
	return on;
}

void markTexFilter(bool set)
{
	if (!texGuardOn())
		return;
	const GLint t = g_boundTex[unitIndex()];
	if (t <= 0)
		return;
	if (static_cast<size_t>(t) >= g_texHasFilter.size())
		g_texHasFilter.resize(static_cast<size_t>(t) + 256, 0);
	g_texHasFilter[static_cast<size_t>(t)] = set ? 1 : 0;
}

bool texHasFilter()
{
	const GLint t = g_boundTex[unitIndex()];
	return t > 0 && static_cast<size_t>(t) < g_texHasFilter.size() && g_texHasFilter[static_cast<size_t>(t)];
}

void ensureTexFilter()
{
	if (!texGuardOn())
		return;
	if (texHasFilter())
		return;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	markTexFilter(true);
}

void checkTexError(const char *what, int w, int h)
{
	if (!texGuardOn())
		return;
	static int reported = 0;
	const GLenum e = glGetError();
	if (e != GL_NO_ERROR && reported < 40)
	{
		++reported;
		netcast::log("gl error 0x%x in %s (%dx%d, tex %d)", static_cast<unsigned>(e), what, w, h,
		             static_cast<int>(g_boundTex[unitIndex()]));
	}
}

void execColor(float r, float g, float b, float a)
{
	if (g_colorValid && g_color[0] == r && g_color[1] == g && g_color[2] == b && g_color[3] == a)
		return;
	glColor4f(r, g, b, a);
	g_color[0] = r; g_color[1] = g; g_color[2] = b; g_color[3] = a;
	g_colorValid = true;
}

// ---------------------------------------------------------------- draws ----
inline const void *offsetPtr(const void *base, int offset)
{
	return reinterpret_cast<const void *>(reinterpret_cast<std::uintptr_t>(base) + static_cast<std::uintptr_t>(offset));
}

// Issue one draw. `base` is a client pointer (no ARRAY_BUFFER bound) or null
// with a buffer bound, in which case attribute pointers are plain offsets.
// Client-array enables are left as they are for the next draw (see state cache).
void setPointers(const void *base, const RenderInterleavedMesh &m)
{
	if (m.hasTexture)
	{
		clientUnit(GL_TEXTURE0);
		glTexCoordPointer(2, GL_FLOAT, m.stride, offsetPtr(base, m.texCoordOffset));
	}
	if (m.hasBrightness)
	{
		clientUnit(GL_TEXTURE1);
		glTexCoordPointer(2, GL_SHORT, m.stride, offsetPtr(base, m.brightnessOffset));
	}
	if (m.hasColor)
		glColorPointer(4, GL_UNSIGNED_BYTE, m.stride, offsetPtr(base, m.colorOffset));
	if (m.hasNormals)
		glNormalPointer(GL_BYTE, m.stride, offsetPtr(base, m.normalOffset));
	glVertexPointer(3, m.positionShort ? GL_SHORT : GL_FLOAT, m.stride, offsetPtr(base, 0));
}

void submitMesh(const void *base, bool fromBuffer, const RenderInterleavedMesh &m)
{
	++netcast::g_statDraws;
	clientArray(g_cs.vertex, true, GL_VERTEX_ARRAY);
	if (m.hasTexture != g_cs.tex0)
	{
		clientUnit(GL_TEXTURE0);
		clientArray(g_cs.tex0, m.hasTexture, GL_TEXTURE_COORD_ARRAY);
	}
	if (m.hasBrightness != g_cs.tex1)
	{
		clientUnit(GL_TEXTURE1);
		clientArray(g_cs.tex1, m.hasBrightness, GL_TEXTURE_COORD_ARRAY);
	}
	clientArray(g_cs.color, m.hasColor, GL_COLOR_ARRAY);
	clientArray(g_cs.normal, m.hasNormals, GL_NORMAL_ARRAY);
	if (m.hasColor)
		g_colorValid = false; // current colour is undefined after a colour-array draw

	if (m.primitive == RenderPrimitive::Quads)
	{
		// Quads are expanded through the shared index buffer, one batch at a time.
		ensureQuadIndices();
		const bool ibo = fromBuffer && quadIbo() != 0;
		if (ibo)
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quadIbo);
		int done = 0;
		const int total = m.count - (m.count % 4);
		while (done < total)
		{
			const int batch = (total - done) < kQuadBatchVertices ? (total - done) : kQuadBatchVertices;
			setPointers(offsetPtr(base, done * m.stride), m);
			glDrawElements(GL_TRIANGLES, (batch / 4) * 6, GL_UNSIGNED_SHORT,
			               ibo ? static_cast<const void *>(nullptr) : static_cast<const void *>(g_quadIdx.data()));
			done += batch;
		}
		if (ibo)
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	else
	{
		setPointers(base, m);
		glDrawArrays(static_cast<GLenum>(renderPrimitiveValue(m.primitive)), 0, m.count);
	}
	(void)fromBuffer;
}

// --------------------------------------------------------- display lists ----
enum class Op : std::uint8_t
{
	Enable, Disable, BlendFunc, DepthMask, DepthFunc, AlphaFunc, CullFace, ColorMask,
	BindTexture, ActiveTexture, MultiTexCoord, Color4, Normal3, ShadeModel,
	PolygonOffset, LineWidth, MatrixMode, LoadIdentity, Push, Pop,
	Translate, Rotate, Scale, Frustum, Ortho, CallList, Draw,
};

struct Cmd
{
	Op op;
	int i0 = 0;
	int i1 = 0;
	float f[4] = {0, 0, 0, 0};
	double d[6] = {0, 0, 0, 0, 0, 0};
};

struct DrawRec
{
	RenderInterleavedMesh layout; // data == nullptr, first == 0
	std::vector<unsigned char> bytes; // only when not buffered
	GLuint vbo = 0;
};

struct DisplayList
{
	std::vector<Cmd> cmds;
	std::vector<DrawRec> draws;
};

std::unordered_map<int, DisplayList> g_lists;
int g_nextList = 1;
DisplayList *g_rec = nullptr;
int g_listDepth = 0;

void freeList(DisplayList &l)
{
	for (DrawRec &d : l.draws)
		if (d.vbo != 0)
			glDeleteBuffers(1, &d.vbo);
	l.draws.clear();
	l.cmds.clear();
}

void execCmd(const DisplayList &list, const Cmd &c);

void callList(int id)
{
	auto it = g_lists.find(id);
	if (it == g_lists.end() || g_listDepth >= kMaxListDepth)
		return;
	++g_listDepth;
	const DisplayList &l = it->second;
	for (const Cmd &c : l.cmds)
		execCmd(l, c);
	--g_listDepth;
}

void execCmd(const DisplayList &list, const Cmd &c)
{
	switch (c.op)
	{
		case Op::Enable:        execEnable(static_cast<GLenum>(c.i0), true); break;
		case Op::Disable:       execEnable(static_cast<GLenum>(c.i0), false); break;
		case Op::BlendFunc:     glBlendFunc(static_cast<GLenum>(c.i0), static_cast<GLenum>(c.i1)); break;
		case Op::DepthMask:     glDepthMask(c.i0 ? GL_TRUE : GL_FALSE); break;
		case Op::DepthFunc:     glDepthFunc(static_cast<GLenum>(c.i0)); break;
		case Op::AlphaFunc:     glAlphaFunc(static_cast<GLenum>(c.i0), c.f[0]); break;
		case Op::CullFace:      glCullFace(static_cast<GLenum>(c.i0)); break;
		case Op::ColorMask:     glColorMask(c.i0 & 1, (c.i0 >> 1) & 1, (c.i0 >> 2) & 1, (c.i0 >> 3) & 1); break;
		case Op::BindTexture:   execBind(c.i0); break;
		case Op::ActiveTexture: execActiveTexture(static_cast<GLenum>(c.i0)); break;
		case Op::MultiTexCoord: glMultiTexCoord4f(static_cast<GLenum>(c.i0), c.f[0], c.f[1], 0.0f, 1.0f); break;
		case Op::Color4:        execColor(c.f[0], c.f[1], c.f[2], c.f[3]); break;
		case Op::Normal3:       glNormal3f(c.f[0], c.f[1], c.f[2]); break;
		case Op::ShadeModel:    glShadeModel(static_cast<GLenum>(c.i0)); break;
		case Op::PolygonOffset: glPolygonOffset(c.f[0], c.f[1]); break;
		case Op::LineWidth:     glLineWidth(c.f[0]); break;
		case Op::MatrixMode:    glMatrixMode(static_cast<GLenum>(c.i0)); break;
		case Op::LoadIdentity:  glLoadIdentity(); break;
		case Op::Push:          glPushMatrix(); break;
		case Op::Pop:           glPopMatrix(); break;
		case Op::Translate:     glTranslatef(c.f[0], c.f[1], c.f[2]); break;
		case Op::Rotate:        glRotatef(c.f[0], c.f[1], c.f[2], c.f[3]); break;
		case Op::Scale:         glScalef(c.f[0], c.f[1], c.f[2]); break;
		case Op::Frustum:       glFrustumf(static_cast<GLfloat>(c.d[0]), static_cast<GLfloat>(c.d[1]), static_cast<GLfloat>(c.d[2]),
		                                   static_cast<GLfloat>(c.d[3]), static_cast<GLfloat>(c.d[4]), static_cast<GLfloat>(c.d[5])); break;
		case Op::Ortho:         glOrthof(static_cast<GLfloat>(c.d[0]), static_cast<GLfloat>(c.d[1]), static_cast<GLfloat>(c.d[2]),
		                                 static_cast<GLfloat>(c.d[3]), static_cast<GLfloat>(c.d[4]), static_cast<GLfloat>(c.d[5])); break;
		case Op::CallList:      callList(c.i0); break;
		case Op::Draw:
		{
			if (c.i0 < 0 || c.i0 >= static_cast<int>(list.draws.size()))
				break;
			const DrawRec &d = list.draws[static_cast<size_t>(c.i0)];
			if (d.vbo != 0)
			{
				glBindBuffer(GL_ARRAY_BUFFER, d.vbo);
				submitMesh(nullptr, true, d.layout);
				glBindBuffer(GL_ARRAY_BUFFER, 0);
			}
			else if (!d.bytes.empty())
			{
				submitMesh(d.bytes.data(), false, d.layout);
			}
			break;
		}
	}
}

inline Cmd *record(Op op)
{
	if (g_rec == nullptr)
		return nullptr;
	g_rec->cmds.push_back(Cmd{op});
	return &g_rec->cmds.back();
}

} // namespace

namespace PcOpenGlRenderBackend
{

void renderEnable(RenderCapability capability)
{
	if (Cmd *c = record(Op::Enable)) { c->i0 = static_cast<int>(toGlCap(capability)); return; }
	execEnable(toGlCap(capability), true);
}

void renderDisable(RenderCapability capability)
{
	if (Cmd *c = record(Op::Disable)) { c->i0 = static_cast<int>(toGlCap(capability)); return; }
	execEnable(toGlCap(capability), false);
}

void renderBlendFunc(RenderBlendFactor source, RenderBlendFactor destination)
{
	if (Cmd *c = record(Op::BlendFunc))
	{
		c->i0 = static_cast<int>(toGlFactor(source));
		c->i1 = static_cast<int>(toGlFactor(destination));
		return;
	}
	glBlendFunc(toGlFactor(source), toGlFactor(destination));
}

void renderDepthMask(bool enabled)
{
	if (Cmd *c = record(Op::DepthMask)) { c->i0 = enabled ? 1 : 0; return; }
	glDepthMask(enabled ? GL_TRUE : GL_FALSE);
}

void renderDepthFunc(RenderCompare function)
{
	if (Cmd *c = record(Op::DepthFunc)) { c->i0 = static_cast<int>(toGlCompare(function)); return; }
	glDepthFunc(toGlCompare(function));
}

void renderAlphaFunc(RenderCompare function, float reference)
{
	if (Cmd *c = record(Op::AlphaFunc)) { c->i0 = static_cast<int>(toGlCompare(function)); c->f[0] = reference; return; }
	glAlphaFunc(toGlCompare(function), reference);
}

void renderCullFace(RenderFace face)
{
	if (Cmd *c = record(Op::CullFace)) { c->i0 = static_cast<int>(toGlFace(face)); return; }
	glCullFace(toGlFace(face));
}

void renderColorMask(bool red, bool green, bool blue, bool alpha)
{
	const int bits = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0);
	if (Cmd *c = record(Op::ColorMask)) { c->i0 = bits; return; }
	glColorMask(red ? GL_TRUE : GL_FALSE, green ? GL_TRUE : GL_FALSE, blue ? GL_TRUE : GL_FALSE, alpha ? GL_TRUE : GL_FALSE);
}

void renderBindTexture(int texture)
{
	if (Cmd *c = record(Op::BindTexture)) { c->i0 = texture; return; }
	execBind(texture);
}

void renderSetActiveTextureUnit(int textureUnit)
{
	if (Cmd *c = record(Op::ActiveTexture)) { c->i0 = textureUnit; return; }
	execActiveTexture(static_cast<GLenum>(textureUnit));
}

void renderSetClientActiveTextureUnit(int textureUnit)
{
	clientUnit(static_cast<GLenum>(textureUnit));
}

void renderSetMultiTextureCoord(int textureUnit, float u, float v)
{
	if (Cmd *c = record(Op::MultiTexCoord)) { c->i0 = textureUnit; c->f[0] = u; c->f[1] = v; return; }
	glMultiTexCoord4f(static_cast<GLenum>(textureUnit), u, v, 0.0f, 1.0f);
}

void renderSetLightmapColors(const std::uint32_t *, int) {}

void renderColor4f(float r, float g, float b, float a)
{
	if (Cmd *c = record(Op::Color4)) { c->f[0] = r; c->f[1] = g; c->f[2] = b; c->f[3] = a; return; }
	execColor(r, g, b, a);
}

void renderColor3f(float r, float g, float b)
{
	renderColor4f(r, g, b, 1.0f);
}

void renderNormal3f(float x, float y, float z)
{
	if (Cmd *c = record(Op::Normal3)) { c->f[0] = x; c->f[1] = y; c->f[2] = z; return; }
	glNormal3f(x, y, z);
}

void renderGenerateTextures(int count, int *textures)
{
	if (count <= 0 || textures == nullptr)
		return;
	glGenTextures(static_cast<GLsizei>(count), reinterpret_cast<GLuint *>(textures));
}

void renderDeleteTextures(int count, const int *textures)
{
	if (count <= 0 || textures == nullptr)
		return;
	glDeleteTextures(static_cast<GLsizei>(count), reinterpret_cast<const GLuint *>(textures));
	// A deleted texture falls back to 0 on every unit that had it bound.
	for (int i = 0; i < count; ++i)
	{
		for (GLint &b : g_boundTex)
			if (b == textures[i])
				b = 0;
		if (texGuardOn() && textures[i] > 0 && static_cast<size_t>(textures[i]) < g_texHasFilter.size())
			g_texHasFilter[static_cast<size_t>(textures[i])] = 0;
	}
}

namespace
{
inline double monoMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec * 1e-6;
}
} // namespace

void renderTextureSubImageRgba(int level, int x, int y, int width, int height, const void *pixels)
{
	const double t0 = monoMs();
	glTexSubImage2D(GL_TEXTURE_2D, level, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	checkTexError("TexSubImage2D", width, height);
	++netcast::g_statUploads;
	netcast::g_statUploadKB += width * height * 4 / 1024.0;
	netcast::g_statUploadMs += monoMs() - t0;
}

void renderTextureImageRgba(int level, int width, int height, const void *pixels)
{
	// ES 1.x requires internalformat == format.
	const double t0 = monoMs();
	if (level == 0)
		ensureTexFilter();
	glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	checkTexError("TexImage2D", width, height);
	++netcast::g_statUploads;
	netcast::g_statUploadKB += width * height * 4 / 1024.0;
	netcast::g_statUploadMs += monoMs() - t0;
}

void renderTextureParameters(bool blur, bool /*mipmaps*/, bool clamp)
{
	markTexFilter(true);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, blur ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, blur ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
}

void renderApplyTextureQuality(bool blur, int /*mipmapLevel*/, bool /*mipmapLinear*/, int /*anisotropy*/)
{
	// No mip chains on this backend (see the file header).
	markTexFilter(true);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, blur ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, blur ? GL_LINEAR : GL_NEAREST);
}

int renderGetMaxAnisotropy() { return 1; }
int renderGetMaxSamples() { return 0; }

bool renderTextureBeginUpload(int, int, int, int, bool, bool, bool, bool) { return true; }
bool renderTextureIsValid(int texture) { return texture > 0; }
void renderResetResources()
{
	for (GLint &b : g_boundTex)
		b = -1;
}

void renderFogf(RenderFogParameter parameter, float value)
{
	switch (parameter)
	{
		case RenderFogParameter::Density: glFogf(GL_FOG_DENSITY, value); break;
		case RenderFogParameter::Start: glFogf(GL_FOG_START, value); break;
		case RenderFogParameter::End: glFogf(GL_FOG_END, value); break;
		case RenderFogParameter::Mode: glFogf(GL_FOG_MODE, value); break;
		case RenderFogParameter::Color:
		case RenderFogParameter::DistanceMode:
			break; // no scalar form / no NV_fog_distance on ES
	}
}

void renderFogi(RenderFogParameter parameter, RenderFogMode value)
{
	if (parameter == RenderFogParameter::DistanceMode)
		return;
	GLfixed mode = GL_LINEAR;
	switch (value)
	{
		case RenderFogMode::Exp: mode = GL_EXP; break;
		case RenderFogMode::Exp2: mode = GL_EXP2; break;
		case RenderFogMode::Linear:
		case RenderFogMode::EyeRadial: mode = GL_LINEAR; break;
	}
	// GL_FOG_MODE takes an enum; some ES drivers only accept it through glFogf,
	// others only through glFogx. Try the float form, then the fixed form.
	glFogf(GL_FOG_MODE, static_cast<GLfloat>(mode));
	if (glGetError() != GL_NO_ERROR)
		glFogx(GL_FOG_MODE, mode);
}

void renderFogColor(const float *values) { glFogfv(GL_FOG_COLOR, values); }

void renderLightfv(int lightIndex, RenderLightParameter parameter, const float *values)
{
	GLenum p = GL_POSITION;
	switch (parameter)
	{
		case RenderLightParameter::Ambient: p = GL_AMBIENT; break;
		case RenderLightParameter::Diffuse: p = GL_DIFFUSE; break;
		case RenderLightParameter::Specular: p = GL_SPECULAR; break;
		case RenderLightParameter::Position: p = GL_POSITION; break;
	}
	glLightfv(GL_LIGHT0 + static_cast<GLenum>(lightIndex), p, values);
}

void renderLightModelAmbient(const float *values) { glLightModelfv(GL_LIGHT_MODEL_AMBIENT, values); }

// ES 1.x has no glColorMaterial: GL_COLOR_MATERIAL always tracks
// GL_AMBIENT_AND_DIFFUSE, which is the only mode the game asks for.
void renderColorMaterial(RenderFace, RenderColorMaterialMode) {}

void renderShadeModel(RenderShadeModel model)
{
	const GLenum m = model == RenderShadeModel::Smooth ? GL_SMOOTH : GL_FLAT;
	if (Cmd *c = record(Op::ShadeModel)) { c->i0 = static_cast<int>(m); return; }
	glShadeModel(m);
}

void renderClear(unsigned int mask)
{
	// glClear honours the colour/depth write masks. If any pass leaves one off, the
	// buffer is not cleared and the old frame shows through (flashing sky).
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glClear(static_cast<GLbitfield>(mask));
}
// "Smooth FPS" asks for a glFinish every frame. On a tile-based GPU that waits for
// the whole frame to be rendered before the CPU may continue, which removes all
// CPU/GPU overlap. The swap already paces the frame, so this is a no-op here.
void renderFinishGpu() {}
void renderSubmitFrame() {}
void renderClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
void renderClearDepth(double depth) { glClearDepthf(static_cast<GLclampf>(depth)); }

void renderPolygonOffset(float factor, float units)
{
	if (Cmd *c = record(Op::PolygonOffset)) { c->f[0] = factor; c->f[1] = units; return; }
	glPolygonOffset(factor, units);
}

void renderLineWidth(float width)
{
	if (Cmd *c = record(Op::LineWidth)) { c->f[0] = width; return; }
	glLineWidth(width);
}

void renderViewport(int x, int y, int width, int height) { glViewport(x, y, width, height); }

void renderGetViewport(int *values)
{
	if (values != nullptr)
		glGetIntegerv(GL_VIEWPORT, values);
}

void renderGetMatrix(RenderMatrixQuery query, float *values)
{
	const GLenum q = query == RenderMatrixQuery::Projection ? GL_PROJECTION_MATRIX
	               : query == RenderMatrixQuery::Texture ? GL_TEXTURE_MATRIX : GL_MODELVIEW_MATRIX;
	glGetFloatv(q, values);
}

const unsigned char *renderGetString(RenderStringQuery query)
{
	const GLenum q = query == RenderStringQuery::Vendor ? GL_VENDOR
	               : query == RenderStringQuery::Renderer ? GL_RENDERER
	               : query == RenderStringQuery::Version ? GL_VERSION : GL_EXTENSIONS;
	return glGetString(q);
}

bool renderSupportsFeature(RenderFeature)
{
	// Fog distance mode, occlusion queries, mipmap control, anisotropy and
	// multisampling are all unavailable or deliberately off here.
	return false;
}

// The game polls this twice a frame. Errors are logged by netcast::swapBuffers()
// instead (first seconds only), so the driver is not synchronised for nothing.
unsigned int renderGetError() { return 0; }
void renderFogHint(RenderHintMode mode) { glHint(GL_FOG_HINT, mode == RenderHintMode::Nicest ? GL_NICEST : GL_FASTEST); }

void renderMatrixMode(RenderMatrixMode mode)
{
	if (Cmd *c = record(Op::MatrixMode)) { c->i0 = static_cast<int>(toGlMatrixMode(mode)); return; }
	glMatrixMode(toGlMatrixMode(mode));
}

void renderLoadIdentity()
{
	if (record(Op::LoadIdentity)) return;
	glLoadIdentity();
}

void renderPushMatrix()
{
	if (record(Op::Push)) return;
	glPushMatrix();
}

void renderPopMatrix()
{
	if (record(Op::Pop)) return;
	glPopMatrix();
}

void renderTranslate(float x, float y, float z)
{
	if (Cmd *c = record(Op::Translate)) { c->f[0] = x; c->f[1] = y; c->f[2] = z; return; }
	glTranslatef(x, y, z);
}

void renderRotate(float angle, float x, float y, float z)
{
	if (Cmd *c = record(Op::Rotate)) { c->f[0] = angle; c->f[1] = x; c->f[2] = y; c->f[3] = z; return; }
	glRotatef(angle, x, y, z);
}

void renderScale(float x, float y, float z)
{
	if (Cmd *c = record(Op::Scale)) { c->f[0] = x; c->f[1] = y; c->f[2] = z; return; }
	glScalef(x, y, z);
}

void renderScaleDouble(double x, double y, double z)
{
	renderScale(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

void renderFrustum(double l, double r, double b, double t, double n, double f)
{
	if (Cmd *c = record(Op::Frustum)) { c->d[0] = l; c->d[1] = r; c->d[2] = b; c->d[3] = t; c->d[4] = n; c->d[5] = f; return; }
	glFrustumf(static_cast<GLfloat>(l), static_cast<GLfloat>(r), static_cast<GLfloat>(b),
	           static_cast<GLfloat>(t), static_cast<GLfloat>(n), static_cast<GLfloat>(f));
}

void renderOrtho(double l, double r, double b, double t, double n, double f)
{
	if (Cmd *c = record(Op::Ortho)) { c->d[0] = l; c->d[1] = r; c->d[2] = b; c->d[3] = t; c->d[4] = n; c->d[5] = f; return; }
	glOrthof(static_cast<GLfloat>(l), static_cast<GLfloat>(r), static_cast<GLfloat>(b),
	         static_cast<GLfloat>(t), static_cast<GLfloat>(n), static_cast<GLfloat>(f));
}

// ------------------------------------------------------- display lists ----
int renderGenerateDisplayLists(int count)
{
	if (count <= 0)
		return 0;
	decideVbo();
	const int first = g_nextList;
	g_nextList += count;
	for (int i = 0; i < count; ++i)
		g_lists[first + i];
	return first;
}

void renderDeleteDisplayLists(int first, int count)
{
	for (int i = 0; i < count; ++i)
	{
		auto it = g_lists.find(first + i);
		if (it == g_lists.end())
			continue;
		freeList(it->second);
		g_lists.erase(it);
	}
}

void renderBeginDisplayList(int list)
{
	auto it = g_lists.find(list);
	if (it == g_lists.end())
		it = g_lists.emplace(list, DisplayList()).first;
	freeList(it->second);
	g_rec = &it->second;
}

void renderEndDisplayList()
{
	if (g_rec == nullptr)
		return;
	if (g_useVbo)
	{
		for (DrawRec &d : g_rec->draws)
		{
			if (d.vbo != 0 || d.bytes.empty())
				continue;
			glGenBuffers(1, &d.vbo);
			glBindBuffer(GL_ARRAY_BUFFER, d.vbo);
			glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(d.bytes.size()), d.bytes.data(), GL_STATIC_DRAW);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			std::vector<unsigned char>().swap(d.bytes);
		}
	}
	g_rec->cmds.shrink_to_fit();
	g_rec = nullptr;
}

void renderCallDisplayList(int list)
{
	if (Cmd *c = record(Op::CallList)) { c->i0 = list; return; }
	callList(list);
}

void renderCallDisplayLists(int count, const int *lists)
{
	for (int i = 0; i < count; ++i)
		renderCallDisplayList(lists[i]);
}

// ------------------------------------------------- occlusion (disabled) ----
void renderGenerateOcclusionQueries(int count, int *queries)
{
	for (int i = 0; i < count; ++i)
		queries[i] = 0;
}
void renderBeginOcclusionQuery(int) {}
void renderEndOcclusionQuery() {}
// Fail open: a query that "completed" with samples is never culled.
bool renderOcclusionQueryResultAvailable(int) { return true; }
unsigned int renderOcclusionQueryResult(int) { return 1u; }

bool renderReadPixelsRgb(int x, int y, int width, int height, void *pixels)
{
	if (pixels == nullptr || width <= 0 || height <= 0)
		return false;
	// ES guarantees only RGBA/UNSIGNED_BYTE readback.
	std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	unsigned char *out = static_cast<unsigned char *>(pixels);
	for (size_t i = 0, n = static_cast<size_t>(width) * static_cast<size_t>(height); i < n; ++i)
	{
		out[i * 3 + 0] = rgba[i * 4 + 0];
		out[i * 3 + 1] = rgba[i * 4 + 1];
		out[i * 3 + 2] = rgba[i * 4 + 2];
	}
	return true;
}

bool renderCopyFramebufferToBoundTexture(int x, int y, int width, int height)
{
	if (width <= 0 || height <= 0)
		return false;
	// Mid-frame framebuffer reads force a tile-based GPU to finish the frame so far,
	// so this is optional (copy_fb=0, the default, makes the menu draw its panorama
	// directly instead of render-to-texture + blur).
	static const bool enabled = netcast::configInt("copy_fb", 0) != 0;
	if (!enabled)
		return false;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, y, width, height);
	const bool ok = glGetError() == GL_NO_ERROR;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	netcast::g_statCopyMs += (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) * 1e-6;
	return ok;
}

void renderSetLegacyPresentationGamma(bool) {}

// --------------------------------------------------------- mesh submit ----
bool renderDrawInterleaved(const RenderInterleavedMesh &mesh)
{
	if (mesh.data == nullptr || mesh.stride <= 0 || mesh.count <= 0)
		return false;

	if (g_rec != nullptr)
	{
		// Inside a display list: copy the vertices now, draw them on replay.
		DrawRec rec;
		rec.layout = mesh;
		rec.layout.data = nullptr;
		const size_t bytes = static_cast<size_t>(mesh.count) * static_cast<size_t>(mesh.stride);
		rec.bytes.resize(bytes);
		std::memcpy(rec.bytes.data(),
		            static_cast<const unsigned char *>(mesh.data) + static_cast<size_t>(mesh.first) * static_cast<size_t>(mesh.stride),
		            bytes);
		rec.layout.first = 0;
		g_rec->draws.push_back(std::move(rec));
		Cmd *c = record(Op::Draw);
		c->i0 = static_cast<int>(g_rec->draws.size()) - 1;
		return true;
	}

	RenderInterleavedMesh m = mesh;
	m.data = nullptr;
	const void *base = static_cast<const unsigned char *>(mesh.data) + static_cast<size_t>(mesh.first) * static_cast<size_t>(mesh.stride);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	submitMesh(base, false, m);
	return true;
}

bool renderCaptureInterleaved(const RenderInterleavedMesh &mesh, RenderCapturedMesh &out, bool append)
{
	if (mesh.data == nullptr || mesh.stride <= 0 || mesh.count <= 0)
		return false;
	if (!append)
		out.clear();
	if (!out.empty() && (out.stride != mesh.stride || out.primitive != mesh.primitive ||
		out.positionShort != mesh.positionShort ||
		out.hasTexture != mesh.hasTexture || (mesh.hasTexture && out.texCoordOffset != mesh.texCoordOffset) ||
		out.hasColor != mesh.hasColor || (mesh.hasColor && out.colorOffset != mesh.colorOffset) ||
		out.hasNormals != mesh.hasNormals || (mesh.hasNormals && out.normalOffset != mesh.normalOffset) ||
		out.hasBrightness != mesh.hasBrightness || (mesh.hasBrightness && out.brightnessOffset != mesh.brightnessOffset)))
		return false;
	if (out.empty())
	{
		out.stride = mesh.stride; out.primitive = mesh.primitive; out.positionShort = mesh.positionShort;
		out.hasTexture = mesh.hasTexture; out.texCoordOffset = mesh.texCoordOffset;
		out.hasColor = mesh.hasColor; out.colorOffset = mesh.colorOffset;
		out.hasNormals = mesh.hasNormals; out.normalOffset = mesh.normalOffset;
		out.hasBrightness = mesh.hasBrightness; out.brightnessOffset = mesh.brightnessOffset;
	}
	const unsigned char *src = static_cast<const unsigned char *>(mesh.data) + static_cast<size_t>(mesh.first) * static_cast<size_t>(mesh.stride);
	const size_t bytes = static_cast<size_t>(mesh.count) * static_cast<size_t>(mesh.stride);
	const size_t old = out.raw.size();
	out.raw.resize(old + (bytes + 3u) / 4u);
	std::memcpy(reinterpret_cast<unsigned char *>(out.raw.data()) + old * 4u, src, bytes);
	out.vertexCount += mesh.count;
	return true;
}

bool renderDrawCaptured(const RenderCapturedMesh &mesh)
{
	if (mesh.empty())
		return false;
	RenderInterleavedMesh view;
	view.data = mesh.raw.data(); view.stride = mesh.stride; view.count = mesh.vertexCount;
	view.primitive = mesh.primitive; view.positionShort = mesh.positionShort;
	view.hasTexture = mesh.hasTexture; view.texCoordOffset = mesh.texCoordOffset;
	view.hasColor = mesh.hasColor; view.colorOffset = mesh.colorOffset;
	view.hasNormals = mesh.hasNormals; view.normalOffset = mesh.normalOffset;
	view.hasBrightness = mesh.hasBrightness; view.brightnessOffset = mesh.brightnessOffset;
	return PcOpenGlRenderBackend::renderDrawInterleaved(view);
}

} // namespace PcOpenGlRenderBackend

#endif // NETCAST_PLATFORM

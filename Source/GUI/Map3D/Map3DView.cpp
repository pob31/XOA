/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    Map3DView implementation — see Map3DView.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "Map3DView.h"

#include <array>
#include <limits>
#include <map>

#include "Parameters/XoaValueTreeState.h"
#include "GUI/ColorScheme.h"
#include "GUI/ColorUtilities.h"

namespace ids = xoa::ids;

namespace xoa::ui
{

//==============================================================================
// GL-thread resources. Created in newOpenGLContextCreated(), destroyed in
// openGLContextClosing() — never from the message thread.
struct Map3DView::GlResources
{
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    std::unique_ptr<juce::OpenGLShaderProgram::Attribute> aPosition, aNormal;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> uMvp, uModel, uColour, uLit;

    GLuint sphereVbo = 0, sphereIbo = 0;
    GLuint cubeVbo = 0, cubeIbo = 0;
    GLuint gridVbo = 0;
    int sphereIndexCount = 0, cubeIndexCount = 0;
    int gridMinorVerts = 0, gridMajorVerts = 0;   // axes follow: 3 segments x 2 verts
    float gridBuiltExtent = -1.0f;
};

//==============================================================================
// Matrix helpers (column-major, OpenGL convention: m[col * 4 + row]).

Map3DView::Mat4 Map3DView::mat4Multiply (const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k)
                acc += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = acc;
        }
    return r;
}

Map3DView::Mat4 Map3DView::mat4Perspective (float fovYDeg, float aspect, float zNear, float zFar)
{
    const float f = 1.0f / std::tan (juce::degreesToRadians (fovYDeg) * 0.5f);
    Mat4 r;
    r.m[0]  = f / juce::jmax (0.01f, aspect);
    r.m[5]  = f;
    r.m[10] = (zFar + zNear) / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * zFar * zNear / (zNear - zFar);
    return r;
}

Map3DView::Mat4 Map3DView::mat4LookAt (const Vec3& eye, const Vec3& target, const Vec3& up)
{
    const Vec3 f = (target - eye).normalised();
    const Vec3 s = f.cross (up).normalised();
    const Vec3 u = s.cross (f);
    Mat4 r;
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;  r.m[12] = -s.dot (eye);
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;  r.m[13] = -u.dot (eye);
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] = f.dot (eye);
    r.m[15] = 1.0f;
    return r;
}

Map3DView::Mat4 Map3DView::mat4Model (const Vec3& t, float scale)
{
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = scale;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    r.m[15] = 1.0f;
    return r;
}

//==============================================================================
// Mesh builders (interleaved position + normal, 6 floats per vertex).

static void buildIcosphere (std::vector<float>& verts, std::vector<juce::uint32>& indices)
{
    const float t = (1.0f + std::sqrt (5.0f)) / 2.0f;
    std::vector<std::array<float, 3>> pos = {
        { -1,  t,  0 }, { 1,  t, 0 }, { -1, -t,  0 }, { 1, -t,  0 },
        {  0, -1,  t }, { 0,  1, t }, {  0, -1, -t }, { 0,  1, -t },
        {  t,  0, -1 }, { t,  0, 1 }, { -t,  0, -1 }, { -t, 0,  1 } };
    for (auto& p : pos)
    {
        const float l = std::sqrt (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        p = { p[0] / l, p[1] / l, p[2] / l };
    }
    std::vector<std::array<juce::uint32, 3>> faces = {
        { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 },
        { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
        { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 },
        { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };

    // One subdivision (80 faces) — plenty for a 15 cm marker sphere.
    std::map<std::pair<juce::uint32, juce::uint32>, juce::uint32> midpoints;
    auto midpoint = [&] (juce::uint32 a, juce::uint32 b) -> juce::uint32
    {
        const std::pair<juce::uint32, juce::uint32> key = std::minmax (a, b);
        const auto it = midpoints.find (key);
        if (it != midpoints.end())
            return it->second;
        std::array<float, 3> m = { pos[a][0] + pos[b][0], pos[a][1] + pos[b][1], pos[a][2] + pos[b][2] };
        const float l = std::sqrt (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        pos.push_back ({ m[0] / l, m[1] / l, m[2] / l });
        const auto idx = (juce::uint32) (pos.size() - 1);
        midpoints[key] = idx;
        return idx;
    };
    std::vector<std::array<juce::uint32, 3>> refined;
    refined.reserve (faces.size() * 4);
    for (const auto& fc : faces)
    {
        const auto ab = midpoint (fc[0], fc[1]), bc = midpoint (fc[1], fc[2]), ca = midpoint (fc[2], fc[0]);
        refined.push_back ({ fc[0], ab, ca });
        refined.push_back ({ fc[1], bc, ab });
        refined.push_back ({ fc[2], ca, bc });
        refined.push_back ({ ab, bc, ca });
    }

    verts.clear();
    verts.reserve (pos.size() * 6);
    for (const auto& p : pos)   // unit sphere: normal == position
    {
        verts.insert (verts.end(), { p[0], p[1], p[2], p[0], p[1], p[2] });
    }
    indices.clear();
    indices.reserve (refined.size() * 3);
    for (const auto& fc : refined)
        indices.insert (indices.end(), { fc[0], fc[1], fc[2] });
}

static void buildCube (std::vector<float>& verts, std::vector<juce::uint32>& indices)
{
    struct Face { float n[3]; float a[3], b[3], c[3], d[3]; };
    const Face facesArr[6] = {
        { {  1, 0, 0 }, { 1, -1, -1 }, { 1,  1, -1 }, { 1,  1,  1 }, { 1, -1,  1 } },
        { { -1, 0, 0 }, { -1,  1, -1 }, { -1, -1, -1 }, { -1, -1,  1 }, { -1,  1,  1 } },
        { { 0,  1, 0 }, {  1,  1, -1 }, { -1,  1, -1 }, { -1,  1,  1 }, {  1,  1,  1 } },
        { { 0, -1, 0 }, { -1, -1, -1 }, {  1, -1, -1 }, {  1, -1,  1 }, { -1, -1,  1 } },
        { { 0, 0,  1 }, { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 } },
        { { 0, 0, -1 }, { -1,  1, -1 }, {  1,  1, -1 }, {  1, -1, -1 }, { -1, -1, -1 } } };

    verts.clear();
    indices.clear();
    juce::uint32 base = 0;
    for (const auto& f : facesArr)
    {
        for (const float* p : { f.a, f.b, f.c, f.d })
            verts.insert (verts.end(), { p[0], p[1], p[2], f.n[0], f.n[1], f.n[2] });
        indices.insert (indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        base += 4;
    }
}

//==============================================================================
Map3DView::Map3DView (XoaValueTreeState& storeIn) : store (storeIn)
{
    setOpaque (true);                     // the GL frame fills every pixel
    setWantsKeyboardFocus (false);        // keys are handled by the owning MapTab

    openGLContext.setRenderer (this);
    openGLContext.setContinuousRepainting (false);
    openGLContext.setComponentPaintingEnabled (true);
    openGLContext.attachTo (*this);
}

Map3DView::~Map3DView()
{
    openGLContext.detach();
}

//==============================================================================
Map3DView::Vec3 Map3DView::OrbitCamera::eye() const
{
    const float az = juce::degreesToRadians (azimuthDeg);
    const float el = juce::degreesToRadians (elevationDeg);
    return target + Vec3 { std::cos (el) * std::cos (az),
                           std::cos (el) * std::sin (az),
                           std::sin (el) } * distance;
}

void Map3DView::cameraBasis (Vec3& forward, Vec3& right, Vec3& up) const
{
    forward = (camera.target - camera.eye()).normalised();
    right   = forward.cross ({ 0, 0, 1 }).normalised();
    up      = right.cross (forward);
}

//==============================================================================
void Map3DView::updateScene (int selectedSource)
{
    lastSelected = selectedSource;

    SceneSnapshot next;
    const auto& col = ColorScheme::get();
    next.background     = col.background;
    next.gridColour     = col.chromeDivider;
    next.textColour     = col.textPrimary;
    next.listenerColour = col.accentGreen;

    float maxAbs = 1.0f;
    auto fp = [this] (const juce::Identifier& id, int ch) { return (float) store.getFloatParameter (id, ch); };

    const int numInputs = store.getNumInputs();
    next.sources.reserve ((size_t) numInputs);
    for (int i = 0; i < numInputs; ++i)
    {
        SourceInstance s;
        s.pos      = { fp (ids::inputPositionX, i), fp (ids::inputPositionY, i), fp (ids::inputPositionZ, i) };
        s.colour   = XoaColorUtilities::getInputColor (i + 1);
        s.selected = (i == selectedSource);
        maxAbs = juce::jmax (maxAbs, std::abs (s.pos.x), std::abs (s.pos.y), std::abs (s.pos.z));
        next.sources.push_back (s);
    }

    const int numSpeakers = store.getNumSpeakers();
    next.speakers.reserve ((size_t) numSpeakers);
    for (int s = 0; s < numSpeakers; ++s)
    {
        const Vec3 p { fp (ids::speakerPositionX, s), fp (ids::speakerPositionY, s), fp (ids::speakerPositionZ, s) };
        maxAbs = juce::jmax (maxAbs, std::abs (p.x), std::abs (p.y), std::abs (p.z));
        next.speakers.push_back (p);
    }

    next.listener = { (float) store.getFloatParameter (ids::listenerX),
                      (float) store.getFloatParameter (ids::listenerY),
                      (float) store.getFloatParameter (ids::listenerZ) };

    rigExtent = maxAbs;
    next.gridExtent = juce::jlimit (5.0f, 50.0f, std::ceil (maxAbs * 1.2f));

    if (! framedOnce)   // frame the rig on the first snapshot after opening
    {
        framedOnce = true;
        camera.distance = juce::jlimit (2.0f, 400.0f, rigExtent * 3.0f);
    }

    next.viewportW = juce::jmax (1, getWidth());
    next.viewportH = juce::jmax (1, getHeight());
    const float aspect = (float) next.viewportW / (float) next.viewportH;
    next.eye = camera.eye();
    next.viewProj = mat4Multiply (mat4Perspective (kFovYDegrees, aspect, 0.1f, 1000.0f),
                                  mat4LookAt (next.eye, camera.target, { 0, 0, 1 }));

    {
        const juce::ScopedLock sl (sceneLock);
        scene = std::move (next);
    }
    repaint();                       // labels
    openGLContext.triggerRepaint();  // the scene itself
}

void Map3DView::rebuildMatrices()
{
    {
        const juce::ScopedLock sl (sceneLock);
        scene.viewportW = juce::jmax (1, getWidth());
        scene.viewportH = juce::jmax (1, getHeight());
        const float aspect = (float) scene.viewportW / (float) scene.viewportH;
        scene.eye = camera.eye();
        scene.viewProj = mat4Multiply (mat4Perspective (kFovYDegrees, aspect, 0.1f, 1000.0f),
                                       mat4LookAt (scene.eye, camera.target, { 0, 0, 1 }));
    }
    repaint();
    openGLContext.triggerRepaint();
}

void Map3DView::frameRig()
{
    camera.target = { 0, 0, 0 };
    camera.distance = juce::jlimit (2.0f, 400.0f, rigExtent * 3.0f);
    rebuildMatrices();
}

void Map3DView::resized()
{
    rebuildMatrices();
}

//==============================================================================
bool Map3DView::projectToScreen (const Vec3& world, juce::Point<float>& screen) const
{
    const auto& m = scene.viewProj.m;
    const float cx = m[0] * world.x + m[4] * world.y + m[8]  * world.z + m[12];
    const float cy = m[1] * world.x + m[5] * world.y + m[9]  * world.z + m[13];
    const float cw = m[3] * world.x + m[7] * world.y + m[11] * world.z + m[15];
    if (cw <= 0.001f)
        return false;   // behind the camera
    screen.x = (cx / cw * 0.5f + 0.5f) * (float) scene.viewportW;
    screen.y = (1.0f - (cy / cw * 0.5f + 0.5f)) * (float) scene.viewportH;
    return true;
}

void Map3DView::paint (juce::Graphics& g)
{
    // Source numbers, composited over the GL frame (message thread — no GL
    // calls; scene is only ever written on this thread).
    g.setColour (scene.textColour);
    g.setFont (juce::FontOptions (11.0f));
    for (size_t i = 0; i < scene.sources.size(); ++i)
    {
        juce::Point<float> p;
        if (projectToScreen (scene.sources[i].pos, p)
            && getLocalBounds().toFloat().expanded (20.0f).contains (p))
            g.drawText (juce::String ((int) i + 1),
                        (int) p.x - 10, (int) p.y - 24, 20, 14, juce::Justification::centred);
    }
}

//==============================================================================
void Map3DView::mouseDown (const juce::MouseEvent& e)
{
    dragStart       = e.position;
    dragStartCamera = camera;
    dragIsPan       = e.mods.isRightButtonDown() || e.mods.isMiddleButtonDown();
    dragMoved       = false;
}

void Map3DView::mouseDrag (const juce::MouseEvent& e)
{
    const auto delta = e.position - dragStart;
    if (delta.getDistanceFromOrigin() > 3.0f)
        dragMoved = true;
    if (! dragMoved)
        return;

    if (dragIsPan)
    {
        // Grab-the-world panning in the camera plane.
        Vec3 f, r, u;
        cameraBasis (f, r, u);
        const float worldPerPixel = 2.0f * dragStartCamera.distance
                                    * std::tan (juce::degreesToRadians (kFovYDegrees) * 0.5f)
                                    / (float) juce::jmax (1, getHeight());
        camera.target = dragStartCamera.target
                        + r * (-delta.x * worldPerPixel)
                        + u * (delta.y * worldPerPixel);
    }
    else
    {
        camera.azimuthDeg   = dragStartCamera.azimuthDeg - delta.x * 0.4f;
        camera.elevationDeg = juce::jlimit (-89.0f, 89.0f, dragStartCamera.elevationDeg - delta.y * 0.4f);
    }
    rebuildMatrices();
}

void Map3DView::mouseUp (const juce::MouseEvent& e)
{
    if (! dragMoved && ! dragIsPan && e.mods.isLeftButtonDown())
    {
        const int hit = pickSource (e.position);
        if (hit >= 0 && onSourceClicked != nullptr)
            onSourceClicked (hit);
    }
}

void Map3DView::mouseDoubleClick (const juce::MouseEvent&)
{
    frameRig();
}

void Map3DView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    camera.distance = juce::jlimit (0.5f, 400.0f,
                                    camera.distance * std::pow (0.9f, wheel.deltaY * 10.0f));
    rebuildMatrices();
}

int Map3DView::pickSource (juce::Point<float> screenPos) const
{
    // Ray through the pixel, from the camera basis — no matrix inversion needed.
    Vec3 f, r, u;
    cameraBasis (f, r, u);
    const float w = (float) juce::jmax (1, getWidth()), h = (float) juce::jmax (1, getHeight());
    const float ndcX = screenPos.x / w * 2.0f - 1.0f;
    const float ndcY = 1.0f - screenPos.y / h * 2.0f;
    const float tanHalf = std::tan (juce::degreesToRadians (kFovYDegrees) * 0.5f);
    const Vec3 dir = (f + r * (ndcX * (w / h) * tanHalf) + u * (ndcY * tanHalf)).normalised();
    const Vec3 eye = camera.eye();

    int best = -1;
    float bestMiss = std::numeric_limits<float>::max();
    for (size_t i = 0; i < scene.sources.size(); ++i)
    {
        const Vec3 toP = scene.sources[i].pos - eye;
        const float along = toP.dot (dir);
        if (along <= 0.0f)
            continue;
        const float miss = (toP - dir * along).length();
        const float grabRadius = juce::jmax (kSourceRadius * 1.6f, 0.02f * along);
        if (miss < grabRadius && miss < bestMiss)
        {
            bestMiss = miss;
            best = (int) i;
        }
    }
    return best;
}

//==============================================================================
// GL thread from here down.

void Map3DView::newOpenGLContextCreated()
{
    using namespace juce::gl;

    gl = std::make_unique<GlResources>();

    // Compatibility-profile GLSL: one program, flat colour with a fixed
    // directional light mixed in for the solid meshes (lit = 1).
    static const char* vertexSrc =
        "attribute vec3 position;\n"
        "attribute vec3 normal;\n"
        "uniform mat4 mvp;\n"
        "uniform mat4 model;\n"
        "uniform vec4 colour;\n"
        "uniform float lit;\n"
        "varying vec4 fragColour;\n"
        "void main()\n"
        "{\n"
        "    gl_Position = mvp * vec4 (position, 1.0);\n"
        "    vec3 n = normalize ((model * vec4 (normal, 0.0)).xyz);\n"
        "    float diff = max (dot (n, normalize (vec3 (0.4, 0.3, 0.85))), 0.0);\n"
        "    float shade = mix (1.0, 0.35 + 0.65 * diff, lit);\n"
        "    fragColour = vec4 (colour.rgb * shade, colour.a);\n"
        "}\n";
    static const char* fragmentSrc =
        "varying vec4 fragColour;\n"
        "void main() { gl_FragColor = fragColour; }\n";

    auto program = std::make_unique<juce::OpenGLShaderProgram> (openGLContext);
    if (! program->addVertexShader (vertexSrc)
        || ! program->addFragmentShader (fragmentSrc)
        || ! program->link())
    {
        jassertfalse;   // shader build failed — renderOpenGL() will no-op
        gl->shader.reset();
        return;
    }
    gl->shader = std::move (program);
    gl->aPosition = std::make_unique<juce::OpenGLShaderProgram::Attribute> (*gl->shader, "position");
    gl->aNormal   = std::make_unique<juce::OpenGLShaderProgram::Attribute> (*gl->shader, "normal");
    gl->uMvp      = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*gl->shader, "mvp");
    gl->uModel    = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*gl->shader, "model");
    gl->uColour   = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*gl->shader, "colour");
    gl->uLit      = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*gl->shader, "lit");

    auto uploadMesh = [] (GLuint& vbo, GLuint& ibo,
                          const std::vector<float>& verts, const std::vector<juce::uint32>& indices)
    {
        glGenBuffers (1, &vbo);
        glBindBuffer (GL_ARRAY_BUFFER, vbo);
        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (verts.size() * sizeof (float)), verts.data(), GL_STATIC_DRAW);
        glGenBuffers (1, &ibo);
        glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData (GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (indices.size() * sizeof (juce::uint32)), indices.data(), GL_STATIC_DRAW);
    };

    std::vector<float> verts;
    std::vector<juce::uint32> indices;
    buildIcosphere (verts, indices);
    uploadMesh (gl->sphereVbo, gl->sphereIbo, verts, indices);
    gl->sphereIndexCount = (int) indices.size();

    buildCube (verts, indices);
    uploadMesh (gl->cubeVbo, gl->cubeIbo, verts, indices);
    gl->cubeIndexCount = (int) indices.size();

    glGenBuffers (1, &gl->gridVbo);
    gl->gridBuiltExtent = -1.0f;   // built lazily against the snapshot extent
}

void Map3DView::openGLContextClosing()
{
    using namespace juce::gl;
    if (gl == nullptr)
        return;
    for (auto* buf : { &gl->sphereVbo, &gl->sphereIbo, &gl->cubeVbo, &gl->cubeIbo, &gl->gridVbo })
        if (*buf != 0)
            glDeleteBuffers (1, buf);
    gl.reset();
}

void Map3DView::renderOpenGL()
{
    using namespace juce::gl;
    if (gl == nullptr || gl->shader == nullptr)
        return;

    SceneSnapshot s;
    {
        const juce::ScopedLock sl (sceneLock);
        s = scene;
    }

    const float scale = (float) openGLContext.getRenderingScale();
    glViewport (0, 0, juce::roundToInt (scale * (float) s.viewportW),
                juce::roundToInt (scale * (float) s.viewportH));

    juce::OpenGLHelpers::clear (s.background);
    glEnable (GL_DEPTH_TEST);
    glDepthFunc (GL_LEQUAL);
    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable (GL_CULL_FACE);

    // Lazily (re)build the floor grid when the rig extent bucket changes.
    if (s.gridExtent != gl->gridBuiltExtent)
    {
        const int extent = (int) s.gridExtent;
        std::vector<float> lines;
        auto addLine = [&lines] (float x0, float y0, float z0, float x1, float y1, float z1)
        {
            lines.insert (lines.end(), { x0, y0, z0, x1, y1, z1 });
        };
        const float e = (float) extent;
        for (int i = -extent; i <= extent; ++i)     // minor (skip majors and axes)
            if (i != 0 && i % 5 != 0)
            {
                addLine ((float) i, -e, 0, (float) i, e, 0);
                addLine (-e, (float) i, 0, e, (float) i, 0);
            }
        gl->gridMinorVerts = (int) lines.size() / 3;
        for (int i = -extent; i <= extent; ++i)     // major every 5 m + the axis lines
            if (i == 0 || i % 5 == 0)
            {
                addLine ((float) i, -e, 0, (float) i, e, 0);
                addLine (-e, (float) i, 0, e, (float) i, 0);
            }
        gl->gridMajorVerts = (int) lines.size() / 3 - gl->gridMinorVerts;
        const float axisLen = juce::jmax (1.0f, e * 0.25f);
        addLine (0, 0, 0.002f, axisLen, 0, 0.002f);   // +X front (red), lifted off the grid
        addLine (0, 0, 0.002f, 0, axisLen, 0.002f);   // +Y left (green)
        addLine (0, 0, 0,      0, 0, axisLen);        // +Z up (blue)

        glBindBuffer (GL_ARRAY_BUFFER, gl->gridVbo);
        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (lines.size() * sizeof (float)), lines.data(), GL_STATIC_DRAW);
        gl->gridBuiltExtent = s.gridExtent;
    }

    gl->shader->use();
    const auto posID  = (GLuint) gl->aPosition->attributeID;
    const bool hasNormal = gl->aNormal != nullptr;

    auto setColour = [this] (juce::Colour c, float alpha = 1.0f)
    {
        gl->uColour->set (c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue(), c.getFloatAlpha() * alpha);
    };
    const Mat4 identityModel = mat4Model ({ 0, 0, 0 }, 1.0f);

    // --- Grid + axes (unlit lines, position-only layout) -------------------
    glBindBuffer (GL_ARRAY_BUFFER, gl->gridVbo);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray (posID);
    glVertexAttribPointer (posID, 3, GL_FLOAT, GL_FALSE, 3 * sizeof (float), nullptr);
    if (hasNormal)
    {
        glDisableVertexAttribArray ((GLuint) gl->aNormal->attributeID);
        glVertexAttrib3f ((GLuint) gl->aNormal->attributeID, 0.0f, 0.0f, 1.0f);
    }
    gl->uLit->set (0.0f);
    gl->uMvp->setMatrix4 (s.viewProj.m, 1, false);
    gl->uModel->setMatrix4 (identityModel.m, 1, false);

    setColour (s.gridColour, 0.35f);
    glDrawArrays (GL_LINES, 0, gl->gridMinorVerts);
    setColour (s.gridColour, 0.8f);
    glDrawArrays (GL_LINES, gl->gridMinorVerts, gl->gridMajorVerts);
    const int axisFirst = gl->gridMinorVerts + gl->gridMajorVerts;
    glLineWidth (2.0f);
    setColour (juce::Colour (0xffd05050)); glDrawArrays (GL_LINES, axisFirst, 2);
    setColour (juce::Colour (0xff50c050)); glDrawArrays (GL_LINES, axisFirst + 2, 2);
    setColour (juce::Colour (0xff5080e0)); glDrawArrays (GL_LINES, axisFirst + 4, 2);
    glLineWidth (1.0f);

    // --- Solid meshes -------------------------------------------------------
    auto bindMesh = [&] (GLuint vbo, GLuint ibo)
    {
        glBindBuffer (GL_ARRAY_BUFFER, vbo);
        glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, ibo);
        glEnableVertexAttribArray (posID);
        glVertexAttribPointer (posID, 3, GL_FLOAT, GL_FALSE, 6 * sizeof (float), nullptr);
        if (hasNormal)
        {
            glEnableVertexAttribArray ((GLuint) gl->aNormal->attributeID);
            glVertexAttribPointer ((GLuint) gl->aNormal->attributeID, 3, GL_FLOAT, GL_FALSE,
                                   6 * sizeof (float), (void*) (3 * sizeof (float)));
        }
    };
    auto drawMeshAt = [&] (int indexCount, const Vec3& at, float meshScale, juce::Colour c, float alpha)
    {
        const Mat4 model = mat4Model (at, meshScale);
        gl->uMvp->setMatrix4 (mat4Multiply (s.viewProj, model).m, 1, false);
        gl->uModel->setMatrix4 (model.m, 1, false);
        setColour (c, alpha);
        glDrawElements (GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
    };

    gl->uLit->set (1.0f);

    bindMesh (gl->cubeVbo, gl->cubeIbo);
    for (const auto& spk : s.speakers)
        drawMeshAt (gl->cubeIndexCount, spk, kSpeakerSize, juce::Colour (0xff909090), 1.0f);

    bindMesh (gl->sphereVbo, gl->sphereIbo);
    drawMeshAt (gl->sphereIndexCount, s.listener, kListenerRadius, s.listenerColour, 1.0f);

    for (const auto& src : s.sources)
        drawMeshAt (gl->sphereIndexCount, src.pos, kSourceRadius, src.colour, 1.0f);

    // Selection halo: a larger translucent shell, drawn without depth writes.
    for (const auto& src : s.sources)
        if (src.selected)
        {
            glDepthMask (GL_FALSE);
            gl->uLit->set (0.0f);
            drawMeshAt (gl->sphereIndexCount, src.pos, kSourceRadius * 1.6f, juce::Colours::white, 0.35f);
            gl->uLit->set (1.0f);
            glDepthMask (GL_TRUE);
        }

    glBindBuffer (GL_ARRAY_BUFFER, 0);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
}

} // namespace xoa::ui

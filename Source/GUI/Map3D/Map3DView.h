/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    Map3DView — the orbitable OpenGL scene behind the Map tab's "3D" toggle:
    sources (spheres, per-input colours), speakers (cubes, read-only), the
    listener sweet-spot, a z=0 floor grid and the origin axes. World frame is
    the canonical XOA frame (+X front, +Y left, +Z up, right-handed, meters).

    Editing model: click a source to select it (ray pick — fires
    onSourceClicked); movement is keyboard-only here (the owning MapTab's
    InputNudger). Left-drag orbits, right/middle-drag pans, wheel zooms,
    double-click reframes the rig.

    Threading: renderOpenGL() (GL thread) reads only a SceneSnapshot copied
    under a lock; updateScene() (message thread) rebuilds the snapshot from the
    store and triggers a repaint. The ValueTree is never touched from the GL
    thread. Source number labels are drawn in paint() (composited over GL)
    using the snapshot's view-projection matrix — no GL calls off the GL thread.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace xoa
{
class XoaValueTreeState;
}

namespace xoa::ui
{

class Map3DView : public juce::Component,
                  private juce::OpenGLRenderer
{
public:
    explicit Map3DView (XoaValueTreeState& storeIn);
    ~Map3DView() override;

    /** Fired on the message thread when a source sphere is clicked (0-based). */
    std::function<void (int)> onSourceClicked;

    /** Message thread: re-snapshot the store + camera and trigger a GL repaint.
        Called by the MapTab refresh tick and after any camera/keyboard change. */
    void updateScene (int selectedSource);

    /** Reset the camera to frame the whole rig. */
    void frameRig();

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    //==========================================================================
    struct Vec3
    {
        float x = 0, y = 0, z = 0;

        Vec3  operator+ (const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        Vec3  operator- (const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        Vec3  operator* (float s) const       { return { x * s, y * s, z * s }; }
        float dot   (const Vec3& o) const     { return x * o.x + y * o.y + z * o.z; }
        Vec3  cross (const Vec3& o) const     { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
        float length() const                  { return std::sqrt (dot (*this)); }
        Vec3  normalised() const              { const float l = length(); return l > 0 ? *this * (1.0f / l) : Vec3(); }
    };

    struct Mat4 { float m[16] = {}; };   // column-major (OpenGL convention)

    static Mat4 mat4Multiply (const Mat4& a, const Mat4& b);                              // a * b
    static Mat4 mat4Perspective (float fovYDeg, float aspect, float zNear, float zFar);
    static Mat4 mat4LookAt (const Vec3& eye, const Vec3& target, const Vec3& up);
    static Mat4 mat4Model (const Vec3& translate, float scale);

    struct SourceInstance
    {
        Vec3 pos;
        juce::Colour colour;
        bool selected = false;
    };

    /** Everything the GL thread (and the label/picking code) needs, copied
        under sceneLock. */
    struct SceneSnapshot
    {
        std::vector<SourceInstance> sources;
        std::vector<Vec3> speakers;
        Vec3  listener;
        float gridExtent = 5.0f;     // half-size of the floor grid, meters
        Mat4  viewProj;
        Vec3  eye;
        juce::Colour background, gridColour, textColour, listenerColour;
        int   viewportW = 1, viewportH = 1;
    };

    struct OrbitCamera
    {
        Vec3  target;
        float azimuthDeg   = -135.0f;   // around +Z, from +X
        float elevationDeg = 30.0f;     // clamped to ±89
        float distance     = 12.0f;     // clamped to [0.5, 400] m

        Vec3 eye() const;
    };

    //==========================================================================
    // juce::OpenGLRenderer (GL thread)
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    //==========================================================================
    // Message thread
    void rebuildMatrices();                       // camera/size -> snapshot.viewProj
    bool projectToScreen (const Vec3& world, juce::Point<float>& screen) const;
    int  pickSource (juce::Point<float> screenPos) const;
    void cameraBasis (Vec3& forward, Vec3& right, Vec3& up) const;

    XoaValueTreeState& store;
    juce::OpenGLContext openGLContext;

    juce::CriticalSection sceneLock;
    SceneSnapshot scene;                          // written: message thread; read: GL thread

    OrbitCamera camera;
    float rigExtent = 2.0f;                       // max |coordinate| seen, for framing
    bool  framedOnce = false;                     // auto-frame on the first snapshot
    int   lastSelected = 0;                       // last selection handed to updateScene

    // Mouse interaction state
    juce::Point<float> dragStart;
    OrbitCamera dragStartCamera;
    bool dragIsPan = false, dragMoved = false;

    // GL resources — created/destroyed on the GL thread only (see .cpp)
    struct GlResources;
    std::unique_ptr<GlResources> gl;

    static constexpr float kFovYDegrees   = 45.0f;
    static constexpr float kSourceRadius  = 0.15f;
    static constexpr float kListenerRadius = 0.2f;
    static constexpr float kSpeakerSize   = 0.12f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Map3DView)
};

} // namespace xoa::ui

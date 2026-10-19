// Screen-drag -> world mappings for the explicit transform tools (condition 4:
// camera gestures and tool gestures are separate; these mappings are pinned).
#include "test_util.h"

#include "../core/mesh/Primitives.h"
#include "../core/operations/TransformMath.h"

using namespace blone;

namespace {

ops::DragParams defaultParams() {
    ops::DragParams p;
    p.azimuthDeg = 0.0f;
    p.elevationDeg = 0.0f;
    p.distance = 8.0f;
    p.fovYDeg = 45.0f;
    p.viewportHeightPx = 1000.0f;
    return p;
}

void testMoveMapping() {
    const ops::DragParams p = defaultParams();
    const float wpp = ops::worldPerPx(p);
    CHECK_NEAR(wpp, 2.0f * 8.0f * 0.41421356f / 1000.0f, 1e-6f);

    // Drag right -> selection moves +X (camera at az=0 looks down -Z).
    const Vec3 right = ops::moveWorldDelta(p, 100.0f, 0.0f);
    CHECK_NEAR(right.x, 100.0f * wpp, 1e-5f);
    CHECK_NEAR(right.y, 0.0f, 1e-6f);
    CHECK_NEAR(right.z, 0.0f, 1e-6f);
    // Drag up (dy negative) -> selection moves +Y.
    const Vec3 up = ops::moveWorldDelta(p, 0.0f, -100.0f);
    CHECK_NEAR(up.y, 100.0f * wpp, 1e-5f);
    // Drag down -> -Y (selection follows the finger).
    const Vec3 down = ops::moveWorldDelta(p, 0.0f, 100.0f);
    CHECK_NEAR(down.y, -100.0f * wpp, 1e-5f);
}

void testRotationAndScaleMappings() {
    const ops::DragParams p = defaultParams();
    const ops::DragRotation r = ops::rotationFromDrag(p, 100.0f, -50.0f);
    CHECK_NEAR(r.degY, -40.0f, 1e-4f);  // -0.4 deg/px horizontal
    CHECK_NEAR(r.degRight, -20.0f, 1e-4f); // +0.4 deg/px vertical
    // Camera right at az=0 is +X.
    CHECK_NEAR(r.axisRight.x, 1.0f, 1e-5f);

    CHECK_NEAR(ops::scaleFromDrag(-100.0f), 1.5f, 1e-4f); // drag up = grow
    CHECK_NEAR(ops::scaleFromDrag(100.0f), 0.5f, 1e-4f);  // drag down = shrink
    CHECK_NEAR(ops::scaleFromDrag(-100000.0f), 20.0f, 1e-4f); // clamped
    CHECK_NEAR(ops::scaleFromDrag(100000.0f), 0.05f, 1e-4f);
}

void testApplyHelpers() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    CHECK(buildPrimitive(PrimType::Cube, p, m));
    const Vec3 before1 = m.vrt(1).pos;
    const Vec3 before5 = m.vrt(5).pos;

    ops::applyMove(m, {1}, Vec3{0.5f, 0.0f, 0.0f});
    CHECK_NEAR(m.vrt(1).pos.x - before1.x, 0.5f, 1e-6f);
    CHECK_NEAR(m.vrt(5).pos.x - before5.x, 0.0f, 1e-6f); // others untouched

    ops::applyRotation(m, {1, 5}, Vec3{0, 0, 0}, Vec3{0, 1, 0}, 90.0f);
    // v1 was moved to (-0.5,-1,1); R_y(90°) maps (x,z) -> (z, -x):
    // (-0.5, ·, 1) -> (1, ·, 0.5).
    CHECK_NEAR(m.vrt(1).pos.x, 1.0f, 1e-5f);
    CHECK_NEAR(m.vrt(1).pos.z, 0.5f, 1e-5f);

    ops::applyScale(m, {1}, Vec3{0, 0, 0}, 2.0f);
    CHECK_NEAR(std::fabs(m.vrt(1).pos.x), 2.0f, 1e-5f);
    CHECK(m.validate());
}

} // namespace

namespace blone::test {
void runTransformMathTests() {
    testMoveMapping();
    testRotationAndScaleMappings();
    testApplyHelpers();
}
} // namespace blone::test

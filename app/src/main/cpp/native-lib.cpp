// JNI bridge between the Kotlin/Compose UI and the C++ core/renderer.
//
// Threading: surfaceCreated/surfaceChanged/drawFrame are invoked from the
// GLSurfaceView render thread, which owns the current OpenGL ES 3.0 context.
// pick() runs via GLSurfaceView.queueEvent (also the GL thread) and performs
// pure CPU math against the camera snapshot of the last rendered frame.

#include <jni.h>

#include "core/Camera.h"
#include "renderer/Renderer.h"

namespace {

blone::Renderer gRenderer;
blone::OrbitCamera gCamera;

inline float clampOrbitElevation(float deg) {
    if (deg > 85.0f) return 85.0f;
    if (deg < -85.0f) return -85.0f;
    return deg;
}

inline float clampDistance(float d) {
    if (d < 2.5f) return 2.5f;
    if (d > 40.0f) return 40.0f;
    return d;
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_surfaceCreated(JNIEnv* /*env*/, jobject /*thiz*/) {
    gRenderer.init();
}

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_surfaceChanged(JNIEnv* /*env*/, jobject /*thiz*/,
                                               jint width, jint height) {
    gRenderer.resize(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_drawFrame(JNIEnv* /*env*/, jobject /*thiz*/,
                                          jfloat azimuthDeg, jfloat elevationDeg,
                                          jfloat distance, jfloat tx, jfloat ty,
                                          jfloat tz, jboolean selected) {
    gCamera.azimuthDeg = azimuthDeg;
    gCamera.elevationDeg = clampOrbitElevation(elevationDeg);
    gCamera.distance = clampDistance(distance);
    gCamera.target = blone::Vec3{tx, ty, tz};
    gRenderer.render(gCamera, selected == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_blone_app_gl_NativeLib_pick(JNIEnv* /*env*/, jobject /*thiz*/,
                                     jfloat xPx, jfloat yPx) {
    return gRenderer.pickAtPixel(xPx, yPx) ? JNI_TRUE : JNI_FALSE;
}

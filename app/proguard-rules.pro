# Blone keeps R8 disabled for now (native code + early phase), but these rules
# are kept for when minification is enabled in a later phase.
# Keep JNI entry points — they are called from C++ by symbol name.
-keepclasseswithmembernames class com.blone.app.gl.NativeLib {
    native <methods>;
}

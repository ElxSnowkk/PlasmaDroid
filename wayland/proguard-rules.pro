# Piperella Wayland Compositor — ProGuard/R8 rules
# Applied during release builds via build.gradle.kts proguardFiles().

# Keep all JNI native method entry points.
# These are called by name from C++ via RegisterNatives or by JNI convention;
# R8 must not rename or remove them.
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep all compositor classes. CompositorEngine, the services and the surface
# providers are referenced by name from C++ and from the Android manifest.
-keep class com.piperella.compositor.** { *; }

# Keep AIDL-generated binder stubs — referenced by name from the ITinywlInput
# and ITinywlMain binder pipeline.
-keep class com.xtr.tinywl.** { *; }

# Keep vendored AIDL types from android.hardware.input.common and inputflinger.
# These are parcelable types serialized across the binder interface.
-keep class com.android.server.inputflinger.** { *; }
-keep class android.hardware.input.common.** { *; }

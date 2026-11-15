import java.util.Properties

plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "org.plasma.wayland"
    compileSdk = 35
    ndkVersion = "29.0.14206865"

    defaultConfig {
        minSdk = 29
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                abiFilters += "arm64-v8a"
                arguments += listOf(
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DANDROID_STL=c++_shared"
                )
                val localProperties = Properties()
                val localPropertiesFile = rootProject.file("local.properties")
                if (localPropertiesFile.isFile) {
                    localPropertiesFile.inputStream().use { localProperties.load(it) }
                }
                val sdkDir = localProperties.getProperty("sdk.dir")
                    ?: System.getenv("ANDROID_SDK_ROOT")
                    ?: System.getenv("ANDROID_HOME")
                    ?: "${System.getProperty("user.home")}/Android/Sdk"
                if (sdkDir.isNotBlank()) arguments += "-DPLASMA_ANDROID_SDK_ROOT=$sdkDir"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    packaging { jniLibs { keepDebugSymbols += "*/arm64-v8a/libpiperella_compositor.so" } }
}

kotlin { compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) } }


// Bundle the prebuilts used by the Wayland server into the AAR.
val copyPrebuiltLibsToAar = tasks.register<Copy>("copyPrebuiltLibsToAar") {
    val prebuiltDir = projectDir.resolve("src/main/cpp/prebuilts/arm64-v8a/lib")
    val jniLibsDir = projectDir.resolve("src/main/jniLibs/arm64-v8a")
    from(prebuiltDir) { include("libwayland-server.so", "libxkbcommon.so") }
    into(jniLibsDir)
    doFirst {
        if (!prebuiltDir.resolve("libwayland-server.so").exists() ||
            !prebuiltDir.resolve("libxkbcommon.so").exists()) {
            throw GradleException("Wayland prebuilts ausentes: libwayland-server.so/libxkbcommon.so")
        }
    }
}

tasks.matching { it.name.startsWith("merge") && it.name.endsWith("JniLibFolders") }.configureEach {
    dependsOn(copyPrebuiltLibsToAar)
}

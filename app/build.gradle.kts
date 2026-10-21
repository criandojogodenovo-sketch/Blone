import java.io.File

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.blone.app"
    compileSdk = 35

    // NDK pinned for reproducible native builds (installed automatically by AGP
    // when missing, both locally and on CI runners).
    ndkVersion = "27.1.12297006"

    defaultConfig {
        applicationId = "com.blone.app"
        minSdk = 26
        targetSdk = 35
        versionCode = 7
        versionName = "0.4.0-fase4"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables { useSupportLibrary = true }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-O2", "-frtti", "-fexceptions")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }

        ndk {
            // Keep the set explicit: two mobile ABIs + x86_64 for emulators.
            abiFilters += listOf("armeabi-v7a", "arm64-v8a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Release signing is driven exclusively by environment variables so that no
    // secret ever lands in this file or in git. When the variables are absent
    // (e.g. local dev without secrets) the release build stays unsigned and CI
    // reports the omission instead of faking a signed artifact.
    signingConfigs {
        create("release") {
            val ksPath: String? = System.getenv("ANDROID_KEYSTORE_PATH")
            val ksPass: String? = System.getenv("ANDROID_KEYSTORE_PASSWORD")
            val alias: String? = System.getenv("ANDROID_KEY_ALIAS")
            val keyPass: String? = System.getenv("ANDROID_KEY_PASSWORD")
            val hasKeystore = !ksPath.isNullOrBlank() && !ksPass.isNullOrBlank() &&
                !alias.isNullOrBlank() && !keyPass.isNullOrBlank() &&
                ksPath.let { it != null && File(it).exists() }
            if (hasKeystore) {
                storeFile = File(ksPath!!)
                storePassword = ksPass
                keyAlias = alias
                keyPassword = keyPass
            }
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = false
            isShrinkResources = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Only attach the release signing config when the keystore really
            // resolved; otherwise produce an unsigned APK that CI refuses to
            // present as signed.
            val ksPath = System.getenv("ANDROID_KEYSTORE_PATH")
            val hasKeystore = !ksPath.isNullOrBlank() && File(ksPath).exists()
            if (hasKeystore) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.activity:activity-compose:1.9.3")

    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")

    testImplementation("junit:junit:4.13.2")
}

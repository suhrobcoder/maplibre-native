plugins {
    id("com.android.application")
}

android {
    namespace = "org.maplibre.gltf.test"
    compileSdk = 34

    defaultConfig {
        applicationId = "org.maplibre.gltf.test"
        minSdk = 23
        targetSdk = 33
    }

    flavorDimensions += "renderer"
    productFlavors {
        create("opengl") {
            dimension = "renderer"
        }
        create("vulkan") {
            dimension = "renderer"
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

kotlin {
    jvmToolchain(17)
}

dependencies {
    // When building inside the maplibre-native monorepo:
    implementation(project(":MapLibreGltfLayer"))
    implementation(project(":MapLibreAndroid"))

    // For standalone/plugin usage (after copying the folder to another
    // project), replace with published Maven coordinates:
    //   implementation("org.maplibre.gl:android-sdk:11.0.0")
    //   implementation("org.maplibre.gltf:maplibre-gltf-layer:0.1.0")

    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("com.google.android.material:material:1.11.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.7.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")
}

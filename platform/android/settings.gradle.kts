pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        maven { url = uri("https://jitpack.io") }
    }
}

plugins {
    id("org.gradle.toolchains.foojay-resolver-convention") version "0.8.0"
}

include(":MapLibreAndroid", ":MapLibreAndroidTestApp", ":MapLibreAndroidLint", ":MapLibreGltfLayer")

// MapLibreGltfLayer ships with its own test app — include only when present.
val gltfTestAppDir = file("MapLibreGltfLayer/test-app")
if (gltfTestAppDir.exists()) {
    include(":MapLibreGltfLayer-test-app")
    project(":MapLibreGltfLayer-test-app").projectDir = gltfTestAppDir
}

rootProject.name = "MapLibreAndroid"

val renderTestProjectDir = file("$rootDir/../../render-test/android")
includeBuild(renderTestProjectDir) {
    name = "renderTestApp"
}

val cppTestProjectDir = file("$rootDir/../../test/android")
includeBuild(cppTestProjectDir) {
    name = "cppUnitTestsApp"
}

includeBuild("./MapLibrePlugin")

buildCache {
    local {
        isEnabled = true
    }
}

plugins {
    id("com.android.library")
    `maven-publish`
}

android {
    namespace = "org.maplibre.gltf"
    ndkVersion = Versions.ndkVersion

    defaultConfig {
        compileSdk = 34
        minSdk = 23
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        externalNativeBuild {
            cmake {
                cppFlags("-std=c++17")
            }
        }
        consumerProguardFiles("proguard-rules.pro")
    }

    // Mirror the consumer "renderer" dimension so variant matching
    // succeeds with MapLibreAndroid's opengl/vulkan/webgpu flavors.
    // When consumed externally (via Maven), the consumer's fallbacks
    // handle mismatches.
    flavorDimensions += "renderer"
    productFlavors {
        create("opengl") {
            dimension = "renderer"
            externalNativeBuild {
                cmake {
                    arguments("-DMLN_WITH_OPENGL=ON")
                }
            }
        }
        create("vulkan") {
            dimension = "renderer"
            externalNativeBuild {
                cmake {
                    arguments("-DMLN_WITH_VULKAN=ON")
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path("src/main/cpp/CMakeLists.txt")
        }
    }

    publishing {
        singleVariant("openglRelease") {
            withSourcesJar()
        }
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
    // MapLibre SDK dependency.
    // When building inside the maplibre-native monorepo, use the project ref.
    // For standalone/plugin usage, replace with a published Maven coordinate:
    //   implementation("org.maplibre.gl:android-sdk:11.0.0")
    compileOnly(project(":MapLibreAndroid"))
    implementation(libs.supportAnnotations)
    androidTestImplementation(libs.junit)
    androidTestImplementation(libs.testRunner)
    androidTestImplementation(libs.testRules)
}

val githubPackagesVersion = providers.gradleProperty("maplibreGithubVersion")
    .getOrElse("${rootProject.file("VERSION").readText().trim()}-gltf.1")

group = "org.maplibre.gltf"
version = githubPackagesVersion

afterEvaluate {
    publishing {
        publications {
            register<MavenPublication>("githubOpenglRelease") {
                from(components["openglRelease"])
                groupId = "org.maplibre.gltf"
                artifactId = "maplibre-gltf-layer"
                version = githubPackagesVersion

                pom.withXml {
                    val pom = asNode()
                    val dependencies = pom.children()
                        .filterIsInstance<groovy.util.Node>()
                        .first { it.name().toString().endsWith("dependencies") }
                    dependencies.appendNode("dependency").apply {
                        appendNode("groupId", "org.maplibre.gl")
                        appendNode("artifactId", "android-sdk-opengl")
                        appendNode("version", githubPackagesVersion)
                        appendNode("scope", "compile")
                    }
                }
            }
        }
    }
}

apply(from = rootProject.file("github-packages.gradle.kts"))

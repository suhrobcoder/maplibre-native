// Phase 0 spike: draws a single standing (vertical) triangle anchored to a
// map coordinate, to prove the CustomLayer projection pipeline end to end.
//
// Coordinate space (verified against src/mbgl/map/transform_state.cpp and
// src/mbgl/util/camera.cpp):
//   * CustomLayerRenderParameters::projectionMatrix maps *pixel* coordinates
//     (web-mercator[0,1] * worldSize, worldSize = 512 * 2^zoom) to clip space.
//   * The matrix's Z column is pre-scaled by pixelsPerMeter internally, so
//     vertex Z is supplied in METERS.
// Anchor-relative rendering: we compose (projection * translate(anchorPx) *
// scale(pixelsPerMeter)) on the CPU in double precision and feed vertices as
// small METER offsets from the anchor. This avoids float32 jitter at high zoom.

#include <GLES3/gl3.h>
#include <android/log.h>
#include <jni.h>

#include <mbgl/style/layers/custom_layer_host.hpp>
#include <mbgl/style/layers/custom_layer_render_parameters.hpp>

#include <array>
#include <cmath>
#include <string>

namespace {

constexpr char LOG_TAG[] = "MapLibreGltfLayer";
constexpr double kTileSize = 512.0;
constexpr double kEarthRadiusM = 6378137.0;
constexpr double kPi = 3.14159265358979323846;

// --- Minimal inline math (no mbgl symbols linked) -------------------------

// Web-mercator projection of a lat/lng (degrees) to normalized [0,1] space.
void mercator01(double lat, double lng, double& outX, double& outY) {
    outX = (lng + 180.0) / 360.0;
    const double latRad = lat * kPi / 180.0;
    outY = 0.5 - std::log(std::tan(kPi / 4.0 + latRad / 2.0)) / (2.0 * kPi);
}

// Column-major 4x4 multiply: out = a * b.
void mat4Multiply(std::array<double, 16>& out,
                  const std::array<double, 16>& a,
                  const std::array<double, 16>& b) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

const GLchar* kVertexShader =
    "#version 300 es\n"
    "layout (location = 0) in vec3 a_pos;\n"
    "uniform highp mat4 u_matrix;\n"
    "void main() { gl_Position = u_matrix * vec4(a_pos, 1.0); }";

const GLchar* kFragmentShader =
    "#version 300 es\n"
    "uniform highp vec4 u_color;\n"
    "out highp vec4 fragColor;\n"
    "void main() { fragColor = u_color; }";

GLuint compileShader(GLenum type, const GLchar* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len > 0 ? len : 1), '\0');
        glGetShaderInfoLog(shader, len, nullptr, log.data());
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "shader compile failed: %s", log.c_str());
    }
    return shader;
}

class GltfSpikeLayer final : public mbgl::style::CustomLayerHost {
public:
    // Anchor for the spike: a recognizable location. TestApp centers here.
    GltfSpikeLayer(double lat, double lng)
        : anchorLat(lat), anchorLng(lng) {}

    void initialize(const mbgl::style::CustomLayerInitParameters&) override {
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, "initialize");

        GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);

        aPos = static_cast<GLuint>(glGetAttribLocation(program, "a_pos"));
        uMatrix = glGetUniformLocation(program, "u_matrix");
        uColor = glGetUniformLocation(program, "u_color");

        // Standing triangle, vertices in METERS relative to the anchor:
        // 300 m wide along east/west, 300 m tall along the up (Z) axis.
        // (Spike scale: large enough to be clearly visible at city zoom.)
        const GLfloat vertices[] = {
            -150.0f, 0.0f, 0.0f,   // base west
            150.0f,  0.0f, 0.0f,   // base east
            0.0f,    0.0f, 300.0f, // apex up
        };
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void render(const mbgl::style::CustomLayerRenderParameters& params) override {
        // --- Compose anchor-relative matrix on the CPU in double precision ---
        double mx, my;
        mercator01(anchorLat, anchorLng, mx, my);
        const double worldSize = kTileSize * std::pow(2.0, params.zoom);
        const double anchorPx = mx * worldSize;
        const double anchorPy = my * worldSize;
        const double latRad = anchorLat * kPi / 180.0;
        const double pixelsPerMeter =
            worldSize / (std::cos(latRad) * 2.0 * kPi * kEarthRadiusM);

        // localToWorld = translate(anchorPx, anchorPy, 0) * scale(ppm, ppm, 1)
        // Horizontal meters -> pixels via ppm; Z left in meters (column-major).
        std::array<double, 16> localToWorld = {
            pixelsPerMeter, 0.0,            0.0, 0.0,
            0.0,            pixelsPerMeter, 0.0, 0.0,
            0.0,            0.0,            1.0, 0.0,
            anchorPx,       anchorPy,       0.0, 1.0,
        };

        // Use the NEAR-CLIPPED projection: fill-extrusion renders with it
        // (fill_extrusion_layer_tweaker.cpp, nearClipped=true), so its depth
        // buffer contents are encoded against that near plane. Rendering with
        // the plain projectionMatrix gives an incompatible depth curve and the
        // model loses/wins depth tests arbitrarily against buildings.
        std::array<double, 16> mvp;
        mat4Multiply(mvp, params.nearClippedProjectionMatrix, localToWorld);

        GLfloat mvpF[16];
        for (int i = 0; i < 16; ++i) {
            mvpF[i] = static_cast<GLfloat>(mvp[i]);
        }

        // --- Save GL state we touch (classic custom-layer corruption source) ---
        GLint prevProgram = 0, prevArrayBuffer = 0;
        GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
        GLboolean prevCull = glIsEnabled(GL_CULL_FACE);
        GLboolean prevBlend = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);

        // --- Draw ---
        glUseProgram(program);
        // Core sets a DEGENERATE glDepthRange(d, d) before calling us — a 2D paint-
        // order trick (paint_parameters.cpp depthModeForSublayer), where
        //   d = depthRangeSize + ((1+currentLayer)*numSublayers + n) * depthEpsilon.
        // That d is strictly GREATER than depthRangeSize, so reusing it as our far
        // bound pushes the model BEHIND the map's 3D content. The map's fill-
        // extrusion buildings use depthModeFor3D range {0, depthRangeSize}. To match
        // their window-depth encoding exactly (shared projectionMatrix), render with
        // far = depthRangeSize, now exposed on the render parameters.
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDepthRangef(0.0f, static_cast<GLfloat>(params.depthRangeSize));
        glDisable(GL_BLEND);      // opaque model; core leaves blend on for translucent pass
        glDisable(GL_CULL_FACE);  // triangle is single-sided; keep both faces
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glEnableVertexAttribArray(aPos);
        glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glUniformMatrix4fv(uMatrix, 1, GL_FALSE, mvpF);
        glUniform4f(uColor, 1.0f, 0.2f, 0.1f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisableVertexAttribArray(aPos);

        // --- Restore GL state ---
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArrayBuffer));
        glUseProgram(static_cast<GLuint>(prevProgram));
        if (prevDepthTest == GL_FALSE) glDisable(GL_DEPTH_TEST);
        if (prevCull == GL_TRUE) glEnable(GL_CULL_FACE);
        if (prevBlend == GL_TRUE) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    }

    void contextLost() override {
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, "contextLost");
        // GL context is gone; drop handles without deleting (invalid names).
        program = 0;
        buffer = 0;
    }

    void deinitialize() override {
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, "deinitialize");
        if (buffer) glDeleteBuffers(1, &buffer);
        if (program) glDeleteProgram(program);
        program = 0;
        buffer = 0;
    }

private:
    const double anchorLat;
    const double anchorLng;

    GLuint program = 0;
    GLuint buffer = 0;
    GLuint aPos = 0;
    GLint uMatrix = -1;
    GLint uColor = -1;
};

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_org_maplibre_gltf_GltfSpikeLayer_nativeCreateHost(JNIEnv*, jobject, jdouble lat, jdouble lng) {
    return reinterpret_cast<jlong>(new GltfSpikeLayer(lat, lng));
}

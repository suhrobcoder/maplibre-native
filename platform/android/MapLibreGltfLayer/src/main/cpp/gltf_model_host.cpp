// Phase 3: CustomLayerHost rendering N glTF model instances anchored to map
// coordinates. GLES 3.0. Depth recipe proven in Phase 0:
//   - layer must sit below the fill-extrusion layer in the style,
//   - MVP built from params.nearClippedProjectionMatrix (matches buildings'
//     depth encoding),
//   - glDepthRangef(0, params.depthRangeSize) + LEQUAL + depth writes.
//
// Threading contract: the Kotlin API (add/update/removeModel) may be called
// from any thread; it only mutates LayerState under its mutex. All GL work
// (upload, draw, delete) happens on the map render thread inside the host
// callbacks. Instances sharing one Model share one GPU copy (refcounted via
// shared_ptr; GPU buffers are freed on the render thread once the last
// instance referencing the model is removed).

#include "anchor_math.hpp"
#include "gltf_model.hpp"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <jni.h>

#include <mbgl/style/layers/custom_layer_host.hpp>
#include <mbgl/style/layers/custom_layer_render_parameters.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace maplibre_gltf;

constexpr char LOG_TAG[] = "MapLibreGltfLayer";

const GLchar* kVertexShader = R"(#version 300 es
layout (location = 0) in vec3 a_pos;
layout (location = 1) in vec3 a_normal;
layout (location = 2) in vec2 a_uv;
uniform highp mat4 u_matrix;
out mediump vec3 v_normal;
out mediump vec2 v_uv;
void main() {
    gl_Position = u_matrix * vec4(a_pos, 1.0);
    v_normal = a_normal;
    v_uv = a_uv;
})";

// Dim, moody lighting for dark-mode maps. Low ambient, directional key,
// and a soft rim to keep edges readable without washing out.
const GLchar* kFragmentShader = R"(#version 300 es
precision mediump float;
uniform vec4 u_baseColor;
uniform bool u_hasTexture;
uniform float u_alphaCutoff;
uniform float u_opacity;
uniform sampler2D u_texture;
in mediump vec3 v_normal;
in mediump vec2 v_uv;
out vec4 fragColor;
void main() {
    vec4 color = u_baseColor;
    if (u_hasTexture) color *= texture(u_texture, v_uv);
    if (u_alphaCutoff > 0.0 && color.a < u_alphaCutoff) discard;
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    // Subtle key light for shape definition
    vec3 keyDir = normalize(vec3(0.5, 0.4, 0.75));
    float key = max(dot(n, keyDir), 0.0);
    // Faint rim to keep edges from merging into background
    vec3 upDir = vec3(0.0, 0.0, 1.0);
    float rim = 0.12 * pow(1.0 - abs(dot(n, upDir)), 2.0);
    float light = 0.06 + 0.35 * key + rim;
    fragColor = vec4(color.rgb * light, color.a * u_opacity);
})";

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

struct Placement {
    double lat = 0, lng = 0;
    double scale = 1, rotationDeg = 0, altitudeM = 0;
    double heightScale = 1;
    float opacity = 1.0f;
};

// Shared between the Kotlin-facing JNI handle and the render-thread host.
// Kotlin mutates instances (under mutex); the host reads them each frame.
struct LayerState {
    struct Instance {
        std::shared_ptr<Model> model;
        Placement placement;
    };
    std::mutex mutex;
    std::map<std::string, Instance> instances;
};

// Transform a point by a column-major 4x4, returning clip-space xyzw.
inline std::array<double, 4> mat4TransformPoint(const std::array<double, 16>& m, double x, double y, double z) {
    return {
        m[0] * x + m[4] * y + m[8] * z + m[12],
        m[1] * x + m[5] * y + m[9] * z + m[13],
        m[2] * x + m[6] * y + m[10] * z + m[14],
        m[3] * x + m[7] * y + m[11] * z + m[15],
    };
}

class GltfMultiModelHost final : public mbgl::style::CustomLayerHost {
public:
    explicit GltfMultiModelHost(std::shared_ptr<LayerState> state_)
        : state(std::move(state_)) {}

    void initialize(const mbgl::style::CustomLayerInitParameters&) override {
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        uMatrix = glGetUniformLocation(program, "u_matrix");
        uBaseColor = glGetUniformLocation(program, "u_baseColor");
        uHasTexture = glGetUniformLocation(program, "u_hasTexture");
        uAlphaCutoff = glGetUniformLocation(program, "u_alphaCutoff");
        uTexture = glGetUniformLocation(program, "u_texture");
        uOpacity = glGetUniformLocation(program, "u_opacity");
    }

    void render(const mbgl::style::CustomLayerRenderParameters& params) override {
        if (!program) return;

        // Snapshot instances under the lock; GL work happens after based on
        // the snapshot (shared_ptr keeps models alive even if removed
        // concurrently).
        struct DrawItem {
            std::shared_ptr<Model> model;
            Placement placement;
        };
        std::vector<DrawItem> items;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            items.reserve(state->instances.size());
            for (const auto& [id, inst] : state->instances) {
                items.push_back({inst.model, inst.placement});
            }
        }

        // Upload models seen for the first time; free GPU copies whose last
        // instance is gone (gpu map holds the only remaining reference).
        for (const auto& item : items) {
            if (gpu.find(item.model.get()) == gpu.end()) uploadModel(item.model);
        }
        for (auto it = gpu.begin(); it != gpu.end();) {
            if (it->second.model.use_count() == 1) {
                releaseGpuModel(it->second);
                it = gpu.erase(it);
            } else {
                ++it;
            }
        }
        if (items.empty()) return;

        // --- Save GL state we touch ---
        GLint prevProgram = 0, prevArrayBuffer = 0, prevVao = 0, prevTexture = 0;
        GLint prevActiveTexture = GL_TEXTURE0;
        GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
        GLboolean prevCull = glIsEnabled(GL_CULL_FACE);
        GLboolean prevBlend = glIsEnabled(GL_BLEND);
        GLboolean prevPolyOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);

        glUseProgram(program);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDepthRangef(0.0f, static_cast<GLfloat>(params.depthRangeSize));
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(4.0f, 4.0f);
        glUniform1i(uTexture, 0);

        // Evaluate animations
        {
            if (!animClockStarted) {
                animStart = std::chrono::steady_clock::now();
                animClockStarted = true;
            }
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - animStart).count();
            for (auto& [ptr, gm] : gpu) {
                if (gm.model && gm.model->hasAnimation()) {
                    evaluateAnimation(*gm.model, elapsed);
                    for (size_t i = 0; i < gm.drawables.size() && i < gm.model->drawables.size(); ++i)
                        gm.drawables[i].transform = gm.model->drawables[i].transform;
                }
            }
        }

        // Collect draw calls: opaque/mask drawn immediately in-order; BLEND
        // materials deferred, sorted back-to-front, drawn with blending and
        // depth writes off (they still depth-test against opaque geometry).
        struct BlendCall {
            const GpuDrawable* gd;
            const Model* model;
            std::array<double, 16> base;
            double depth; // clip-space z/w of the drawable center; larger = farther
            float opacity;
        };
        std::vector<BlendCall> blendCalls;

        for (const auto& item : items) {
            const GpuModel& gm = gpu.at(item.model.get());

            // anchor (meter offsets, Z up) -> clip. Then per-drawable:
            // anchor * placement * nodeTransform (node transform baked into
            // each GpuDrawable at upload).
            const std::array<double, 16> anchorMvp = anchorMatrix(
                params.nearClippedProjectionMatrix, item.placement.lat, item.placement.lng, params.zoom);

            // Pixel space: +X east, +Y SOUTH, +Z up — a left-handed frame
            // w.r.t. the physical world. To embed the right-handed glTF model
            // without a reflection (a mirrored model appears to counter-rotate:
            // spinning the map 360° turns it 720° relative to streets), map:
            //   glTF +X -> east (+x), glTF +Y (up) -> up (+z), glTF +Z -> south (+y).
            const double s = item.placement.scale;
            const double hs = s * item.placement.heightScale;
            const double r = item.placement.rotationDeg * kPi / 180.0;
            const double cr = std::cos(r), sr = std::sin(r);
            // placement = translateZ(altitude) * rotZ(r) * axisSwap * scale(s, s, s*hs)
            const std::array<double, 16> placement = {
                s * cr,
                s * sr,
                0.0,
                0.0, // X column: glTF right
                0.0,
                0.0,
                hs,
                0.0, // Y column: glTF up -> map Z (height-scaled)
                -s * sr,
                s * cr,
                0.0,
                0.0, // Z column: glTF back -> map south
                0.0,
                0.0,
                item.placement.altitudeM,
                1.0,
            };

            std::array<double, 16> base;
            mat4Multiply(base, anchorMvp, placement);

            if (isCulled(base, gm)) continue;
            if (hs <= 0.0) continue; // fully flattened — skip rendering

            const float opacity = item.placement.opacity;
            glUniform1f(uOpacity, opacity);
            const bool translucent = opacity < 1.0f;
            if (translucent) {
                glEnable(GL_BLEND);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }

            for (const auto& gd : gm.drawables) {
                const Material& mat = gd.material >= 0 ? item.model->materials[gd.material] : kDefaultMaterial;
                if (mat.alphaMode == AlphaMode::Blend) {
                    const auto c = mat4TransformPoint(base, gd.center[0], gd.center[1], gd.center[2]);
                    const double w = c[3] != 0.0 ? c[3] : 1.0;
                    blendCalls.push_back({&gd, item.model.get(), base, c[2] / w, opacity});
                    continue;
                }
                drawOne(gd, mat, base, translucent);
            }
        }

        if (!blendCalls.empty()) {
            std::sort(blendCalls.begin(), blendCalls.end(), [](const BlendCall& a, const BlendCall& b) {
                return a.depth > b.depth;
            });
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            for (const auto& call : blendCalls) {
                glUniform1f(uOpacity, call.opacity);
                const Material& mat = call.gd->material >= 0 ? call.model->materials[call.gd->material]
                                                             : kDefaultMaterial;
                drawOne(*call.gd, mat, call.base);
            }
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        // --- Restore GL state ---
        glBindVertexArray(static_cast<GLuint>(prevVao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArrayBuffer));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTexture));
        glActiveTexture(static_cast<GLenum>(prevActiveTexture));
        glUseProgram(static_cast<GLuint>(prevProgram));
        if (prevDepthTest == GL_FALSE) glDisable(GL_DEPTH_TEST);
        if (prevCull == GL_TRUE)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (prevBlend == GL_TRUE)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        if (prevPolyOffset == GL_TRUE)
            glEnable(GL_POLYGON_OFFSET_FILL);
        else
            glDisable(GL_POLYGON_OFFSET_FILL);
    }

    void contextLost() override {
        // GL context gone: names invalid; keep CPU-side models for re-upload.
        program = 0;
        gpu.clear();
    }

    void deinitialize() override {
        for (auto& [ptr, gm] : gpu) releaseGpuModel(gm);
        gpu.clear();
        if (program) glDeleteProgram(program);
        program = 0;
    }

private:
    struct GpuDrawable {
        GLuint vao = 0, vbo = 0, ebo = 0;
        GLuint texture = 0;
        GLsizei indexCount = 0;
        int material = -1;
        int nodeIndex = -1;
        std::array<double, 16> transform;
        std::array<double, 3> center{0, 0, 0};
    };

    struct GpuModel {
        std::shared_ptr<Model> model;
        std::vector<GpuDrawable> drawables;
        std::vector<GLuint> textures;
        std::array<double, 3> bboxMin{0, 0, 0}, bboxMax{0, 0, 0};
    };

    // Issues one indexed draw with the material's uniforms and cull state.
    // When forceCullBack is true, back faces are always culled regardless of
    // material's doubleSided flag (used to prevent z-fighting for translucent
    // rendering where interior faces would otherwise surface-fight).
    void drawOne(const GpuDrawable& gd,
                 const Material& mat,
                 const std::array<double, 16>& base,
                 bool forceCullBack = false) const {
        std::array<double, 16> mvp;
        mat4Multiply(mvp, base, gd.transform);
        GLfloat mvpF[16];
        for (int i = 0; i < 16; ++i) mvpF[i] = static_cast<GLfloat>(mvp[i]);
        glUniformMatrix4fv(uMatrix, 1, GL_FALSE, mvpF);

        glUniform4fv(uBaseColor, 1, mat.baseColorFactor.data());
        const bool hasTex = gd.texture != 0;
        glUniform1i(uHasTexture, hasTex ? 1 : 0);
        glUniform1f(uAlphaCutoff, mat.alphaMode == AlphaMode::Mask ? mat.alphaCutoff : 0.0f);
        if (hasTex) glBindTexture(GL_TEXTURE_2D, gd.texture);

        if (mat.doubleSided && !forceCullBack) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        }

        glBindVertexArray(gd.vao);
        glDrawElements(GL_TRIANGLES, gd.indexCount, GL_UNSIGNED_INT, nullptr);
    }

    static inline const Material kDefaultMaterial{};

    // True when the model's AABB (8 corners through mvp) is fully outside one
    // clip plane. Conservative: never culls a visible model.
    static bool isCulled(const std::array<double, 16>& mvp, const GpuModel& gm) {
        int outside[6] = {0, 0, 0, 0, 0, 0};
        for (int c = 0; c < 8; ++c) {
            const double x = (c & 1) ? gm.bboxMax[0] : gm.bboxMin[0];
            const double y = (c & 2) ? gm.bboxMax[1] : gm.bboxMin[1];
            const double z = (c & 4) ? gm.bboxMax[2] : gm.bboxMin[2];
            const auto p = mat4TransformPoint(mvp, x, y, z);
            if (p[0] < -p[3]) ++outside[0];
            if (p[0] > p[3]) ++outside[1];
            if (p[1] < -p[3]) ++outside[2];
            if (p[1] > p[3]) ++outside[3];
            if (p[2] < -p[3]) ++outside[4];
            if (p[2] > p[3]) ++outside[5];
        }
        for (int i = 0; i < 6; ++i) {
            if (outside[i] == 8) return true;
        }
        return false;
    }

    void uploadModel(const std::shared_ptr<Model>& model) {
        GpuModel gm;
        gm.model = model;

        gm.textures.assign(model->textures.size(), 0);
        for (size_t i = 0; i < model->textures.size(); ++i) {
            const Texture& tex = model->textures[i];
            glGenTextures(1, &gm.textures[i]);
            glBindTexture(GL_TEXTURE_2D, gm.textures[i]);
            glTexImage2D(
                GL_TEXTURE_2D, 0, GL_RGBA8, tex.width, tex.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex.rgba.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        bool bboxInit = false;
        gm.drawables.reserve(model->drawables.size());
        for (const Drawable& d : model->drawables) {
            GpuDrawable gd;
            gd.transform = d.transform;
            gd.material = d.material;
            gd.nodeIndex = d.nodeIndex;
            gd.indexCount = static_cast<GLsizei>(d.indices.size());
            if (d.material >= 0) {
                const int texIdx = model->materials[d.material].baseColorTexture;
                if (texIdx >= 0) gd.texture = gm.textures[texIdx];
            }

            // Fold vertices (through the node transform) into the model AABB
            // and this drawable's own bbox (for the blend sort key).
            std::array<double, 3> dMin{0, 0, 0}, dMax{0, 0, 0};
            bool dInit = false;
            for (size_t v = 0; v + 2 < d.vertices.size(); v += kFloatsPerVertex) {
                const auto p = mat4TransformPoint(d.transform, d.vertices[v], d.vertices[v + 1], d.vertices[v + 2]);
                if (!dInit) {
                    dMin = {p[0], p[1], p[2]};
                    dMax = dMin;
                    dInit = true;
                } else {
                    for (int i = 0; i < 3; ++i) {
                        dMin[i] = std::min(dMin[i], p[i]);
                        dMax[i] = std::max(dMax[i], p[i]);
                    }
                }
                if (!bboxInit) {
                    gm.bboxMin = {p[0], p[1], p[2]};
                    gm.bboxMax = {p[0], p[1], p[2]};
                    bboxInit = true;
                } else {
                    for (int i = 0; i < 3; ++i) {
                        gm.bboxMin[i] = std::min(gm.bboxMin[i], p[i]);
                        gm.bboxMax[i] = std::max(gm.bboxMax[i], p[i]);
                    }
                }
            }
            for (int i = 0; i < 3; ++i) gd.center[i] = (dMin[i] + dMax[i]) * 0.5;

            glGenVertexArrays(1, &gd.vao);
            glBindVertexArray(gd.vao);
            glGenBuffers(1, &gd.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, gd.vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(d.vertices.size() * sizeof(float)),
                         d.vertices.data(),
                         GL_STATIC_DRAW);
            glGenBuffers(1, &gd.ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gd.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(d.indices.size() * sizeof(uint32_t)),
                         d.indices.data(),
                         GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kVertexStrideBytes, reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(
                1, 3, GL_FLOAT, GL_FALSE, kVertexStrideBytes, reinterpret_cast<void*>(3 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(
                2, 2, GL_FLOAT, GL_FALSE, kVertexStrideBytes, reinterpret_cast<void*>(6 * sizeof(float)));
            gm.drawables.push_back(gd);
        }
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        __android_log_print(ANDROID_LOG_INFO,
                            LOG_TAG,
                            "uploaded model: %zu drawables, %zu textures",
                            gm.drawables.size(),
                            gm.textures.size());
        gpu.emplace(model.get(), std::move(gm));
    }

    static void releaseGpuModel(GpuModel& gm) {
        for (auto& gd : gm.drawables) {
            if (gd.vao) glDeleteVertexArrays(1, &gd.vao);
            if (gd.vbo) glDeleteBuffers(1, &gd.vbo);
            if (gd.ebo) glDeleteBuffers(1, &gd.ebo);
        }
        if (!gm.textures.empty()) {
            glDeleteTextures(static_cast<GLsizei>(gm.textures.size()), gm.textures.data());
        }
        gm.drawables.clear();
        gm.textures.clear();
    }

    std::shared_ptr<LayerState> state;

    GLuint program = 0;
    GLint uMatrix = -1, uBaseColor = -1, uHasTexture = -1, uAlphaCutoff = -1, uTexture = -1, uOpacity = -1;
    std::map<Model*, GpuModel> gpu;
    std::chrono::steady_clock::time_point animStart;
    bool animClockStarted = false;
};

} // namespace

// JNI surface for org.maplibre.gltf.GltfModelLayer. The Kotlin object owns a
// shared_ptr<LayerState>* (freed by nativeDestroyState); the host created by
// nativeCreateHost holds a second reference, so either side may die first.

namespace {
inline LayerState* stateOf(jlong handle) {
    return reinterpret_cast<std::shared_ptr<LayerState>*>(handle)->get();
}
inline Placement placementOf(jdouble lat,
                             jdouble lng,
                             jdouble scale,
                             jdouble rotationDeg,
                             jdouble altitudeM,
                             jfloat opacity,
                             jdouble heightScale = 1.0) {
    return Placement{lat, lng, scale, rotationDeg, altitudeM, heightScale, opacity};
}
} // namespace

extern "C" JNIEXPORT jlong JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeCreateState(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(new std::shared_ptr<LayerState>(std::make_shared<LayerState>()));
}

extern "C" JNIEXPORT void JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeDestroyState(JNIEnv*,
                                                                                           jclass,
                                                                                           jlong stateHandle) {
    delete reinterpret_cast<std::shared_ptr<LayerState>*>(stateHandle);
}

extern "C" JNIEXPORT jlong JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeCreateHost(JNIEnv*,
                                                                                          jclass,
                                                                                          jlong stateHandle) {
    return reinterpret_cast<jlong>(
        new GltfMultiModelHost(*reinterpret_cast<std::shared_ptr<LayerState>*>(stateHandle)));
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeAddModel(JNIEnv* env,
                                                                                           jclass,
                                                                                           jlong stateHandle,
                                                                                           jstring jid,
                                                                                           jlong modelHandle,
                                                                                           jdouble lat,
                                                                                           jdouble lng,
                                                                                           jdouble scale,
                                                                                           jdouble rotationDeg,
                                                                                           jdouble altitudeM,
                                                                                           jfloat opacity,
                                                                                           jdouble heightScale) {
    // Takes ownership of the model (Kotlin side clears its pointer).
    std::shared_ptr<Model> model(reinterpret_cast<Model*>(modelHandle));
    const char* idChars = env->GetStringUTFChars(jid, nullptr);
    std::string id(idChars);
    env->ReleaseStringUTFChars(jid, idChars);

    LayerState* state = stateOf(stateHandle);
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto [it, inserted] = state->instances.emplace(
        std::move(id),
        LayerState::Instance{std::move(model),
                             placementOf(lat, lng, scale, rotationDeg, altitudeM, opacity, heightScale)});
    return inserted ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeAddInstance(JNIEnv* env,
                                                                                              jclass,
                                                                                              jlong stateHandle,
                                                                                              jstring jid,
                                                                                              jstring jsourceId,
                                                                                              jdouble lat,
                                                                                              jdouble lng,
                                                                                              jdouble scale,
                                                                                              jdouble rotationDeg,
                                                                                              jdouble altitudeM,
                                                                                              jfloat opacity,
                                                                                              jdouble heightScale) {
    const char* idChars = env->GetStringUTFChars(jid, nullptr);
    std::string id(idChars);
    env->ReleaseStringUTFChars(jid, idChars);
    const char* srcChars = env->GetStringUTFChars(jsourceId, nullptr);
    std::string sourceId(srcChars);
    env->ReleaseStringUTFChars(jsourceId, srcChars);

    LayerState* state = stateOf(stateHandle);
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto src = state->instances.find(sourceId);
    if (src == state->instances.end()) return JNI_FALSE;
    const auto [it, inserted] = state->instances.emplace(
        std::move(id),
        LayerState::Instance{src->second.model,
                             placementOf(lat, lng, scale, rotationDeg, altitudeM, opacity, heightScale)});
    return inserted ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeUpdateModel(JNIEnv* env,
                                                                                              jclass,
                                                                                              jlong stateHandle,
                                                                                              jstring jid,
                                                                                              jdouble lat,
                                                                                              jdouble lng,
                                                                                              jdouble scale,
                                                                                              jdouble rotationDeg,
                                                                                              jdouble altitudeM,
                                                                                              jfloat opacity,
                                                                                              jdouble heightScale) {
    const char* idChars = env->GetStringUTFChars(jid, nullptr);
    std::string id(idChars);
    env->ReleaseStringUTFChars(jid, idChars);

    LayerState* state = stateOf(stateHandle);
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->instances.find(id);
    if (it == state->instances.end()) return JNI_FALSE;
    it->second.placement = placementOf(lat, lng, scale, rotationDeg, altitudeM, opacity, heightScale);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeRemoveModel(JNIEnv* env,
                                                                                              jclass,
                                                                                              jlong stateHandle,
                                                                                              jstring jid) {
    const char* idChars = env->GetStringUTFChars(jid, nullptr);
    std::string id(idChars);
    env->ReleaseStringUTFChars(jid, idChars);

    LayerState* state = stateOf(stateHandle);
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->instances.erase(id) > 0 ? JNI_TRUE : JNI_FALSE;
}

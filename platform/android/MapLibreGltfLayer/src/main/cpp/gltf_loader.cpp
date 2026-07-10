#include "gltf_model.hpp"

#define CGLTF_IMPLEMENTATION
#include "vendor/cgltf.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "vendor/stb_image.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace maplibre_gltf {
namespace {

const char* cgltfResultName(cgltf_result r) {
    switch (r) {
        case cgltf_result_success: return "success";
        case cgltf_result_data_too_short: return "data too short";
        case cgltf_result_unknown_format: return "unknown format";
        case cgltf_result_invalid_json: return "invalid json";
        case cgltf_result_invalid_gltf: return "invalid gltf";
        case cgltf_result_invalid_options: return "invalid options";
        case cgltf_result_file_not_found: return "file not found";
        case cgltf_result_io_error: return "io error";
        case cgltf_result_out_of_memory: return "out of memory";
        case cgltf_result_legacy_gltf: return "legacy gltf (1.0) unsupported";
        default: return "unknown error";
    }
}

std::array<double, 16> matMul(const std::array<double, 16>& a, const std::array<double, 16>& b) {
    std::array<double, 16> out{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[col * 4 + k];
            out[col * 4 + row] = sum;
        }
    }
    return out;
}

std::array<double, 16> identity() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

std::array<double, 16> nodeLocalTransform(const cgltf_node& node) {
    cgltf_float m[16];
    cgltf_node_transform_local(&node, m);
    std::array<double, 16> out;
    for (int i = 0; i < 16; ++i) out[i] = m[i];
    return out;
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::vector<uint8_t> bytes;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return bytes;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    if (len > 0) {
        bytes.resize(static_cast<size_t>(len));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
    }
    std::fclose(f);
    return bytes;
}

int decodeTexture(Model& model,
                  std::unordered_map<const cgltf_image*, int>& cache,
                  const cgltf_image* image,
                  const std::string& basePath) {
    if (!image) return -1;
    auto it = cache.find(image);
    if (it != cache.end()) return it->second;

    const uint8_t* bytes = nullptr;
    size_t size = 0;
    std::vector<uint8_t> owned;
    if (image->buffer_view && image->buffer_view->buffer && image->buffer_view->buffer->data) {
        bytes = static_cast<const uint8_t*>(image->buffer_view->buffer->data) + image->buffer_view->offset;
        size = image->buffer_view->size;
    } else if (image->uri) {
        if (std::strncmp(image->uri, "data:", 5) == 0) {
            const char* comma = std::strchr(image->uri, ',');
            if (comma) {
                const char* b64 = comma + 1;
                size_t b64len = std::strlen(b64);
                size_t pad = 0;
                while (pad < 2 && b64len > pad && b64[b64len - 1 - pad] == '=') ++pad;
                const size_t decodedSize = b64len >= 4 ? b64len / 4 * 3 - pad : 0;
                void* out = nullptr;
                cgltf_options opts{};
                if (decodedSize > 0 &&
                    cgltf_load_buffer_base64(&opts, decodedSize, b64, &out) == cgltf_result_success) {
                    owned.assign(static_cast<uint8_t*>(out), static_cast<uint8_t*>(out) + decodedSize);
                    std::free(out);
                    bytes = owned.data();
                    size = owned.size();
                }
            }
        } else if (!basePath.empty()) {
            std::string uri(image->uri);
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            owned = readFileBytes(basePath + "/" + uri);
            if (!owned.empty()) {
                bytes = owned.data();
                size = owned.size();
            }
        }
    }

    int result = -1;
    if (bytes && size > 0) {
        int w = 0, h = 0, comp = 0;
        stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 4);
        if (pixels) {
            Texture tex;
            tex.width = w;
            tex.height = h;
            tex.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
            stbi_image_free(pixels);
            model.textures.push_back(std::move(tex));
            result = static_cast<int>(model.textures.size()) - 1;
        }
    }
    cache.emplace(image, result);
    return result;
}

bool appendPrimitive(Model& model,
                     const cgltf_primitive& prim,
                     const std::array<double, 16>& worldTransform,
                     const std::unordered_map<const cgltf_material*, int>& materialIndex,
                     int nodeIndex,
                     std::string& outError) {
    if (prim.type != cgltf_primitive_type_triangles) return true;

    const cgltf_accessor* pos = nullptr;
    const cgltf_accessor* normal = nullptr;
    const cgltf_accessor* uv = nullptr;
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        const cgltf_attribute& attr = prim.attributes[i];
        if (attr.type == cgltf_attribute_type_position) pos = attr.data;
        else if (attr.type == cgltf_attribute_type_normal) normal = attr.data;
        else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv = attr.data;
    }
    if (!pos) {
        outError = "primitive has no POSITION attribute";
        return false;
    }

    const cgltf_size vertexCount = pos->count;
    Drawable drawable;
    drawable.transform = worldTransform;
    drawable.nodeIndex = nodeIndex;
    drawable.vertices.resize(vertexCount * kFloatsPerVertex, 0.0f);

    for (cgltf_size v = 0; v < vertexCount; ++v) {
        float* dst = drawable.vertices.data() + v * kFloatsPerVertex;
        cgltf_accessor_read_float(pos, v, dst, 3);
        if (normal && v < normal->count) {
            cgltf_accessor_read_float(normal, v, dst + 3, 3);
        } else {
            dst[5] = 1.0f;
        }
        if (uv && v < uv->count) {
            cgltf_accessor_read_float(uv, v, dst + 6, 2);
        }
    }

    if (prim.indices) {
        drawable.indices.resize(prim.indices->count);
        for (cgltf_size i = 0; i < prim.indices->count; ++i) {
            drawable.indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
        }
    } else {
        drawable.indices.resize(vertexCount);
        for (cgltf_size i = 0; i < vertexCount; ++i) drawable.indices[i] = static_cast<uint32_t>(i);
    }

    if (prim.material) {
        auto it = materialIndex.find(prim.material);
        if (it != materialIndex.end()) drawable.material = it->second;
    }

    model.drawables.push_back(std::move(drawable));
    return true;
}

void buildAnimNodes(Model& model, cgltf_data* data) {
    model.animNodes.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& node = data->nodes[i];
        model.animNodes[i].baseLocal = nodeLocalTransform(node);
        model.animNodes[i].parent = -1;
        if (node.has_translation) {
            model.animNodes[i].baseT = {node.translation[0], node.translation[1], node.translation[2]};
        }
        if (node.has_rotation) {
            model.animNodes[i].baseR = {node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]};
        }
        if (node.has_scale) {
            model.animNodes[i].baseS = {node.scale[0], node.scale[1], node.scale[2]};
        }
        // Find parent
        for (cgltf_size p = 0; p < data->nodes_count; ++p) {
            if (p == i) continue;
            for (cgltf_size c = 0; c < data->nodes[p].children_count; ++c) {
                if (data->nodes[p].children[c] == &node) {
                    model.animNodes[i].parent = static_cast<int>(p);
                    break;
                }
            }
            if (model.animNodes[i].parent >= 0) break;
        }
    }
}

void buildAnimChannels(Model& model, cgltf_data* data) {
    for (cgltf_size a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& anim = data->animations[a];
        for (cgltf_size c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& channel = anim.channels[c];
            const cgltf_animation_sampler& sampler = *channel.sampler;

            AnimChannel ac;
            ac.nodeIndex = static_cast<int>(channel.target_node - &data->nodes[0]);

            if (channel.target_path == cgltf_animation_path_type_translation)
                ac.path = AnimChannel::Translation;
            else if (channel.target_path == cgltf_animation_path_type_rotation)
                ac.path = AnimChannel::Rotation;
            else if (channel.target_path == cgltf_animation_path_type_scale)
                ac.path = AnimChannel::Scale;
            else
                continue;

            const cgltf_accessor* input = sampler.input;
            ac.times.resize(input->count);
            for (cgltf_size k = 0; k < input->count; ++k)
                cgltf_accessor_read_float(input, k, &ac.times[k], 1);

            const cgltf_accessor* output = sampler.output;
            int components = (ac.path == AnimChannel::Rotation) ? 4 : 3;
            ac.values.resize(output->count * components);
            for (cgltf_size k = 0; k < output->count; ++k)
                cgltf_accessor_read_float(output, k, &ac.values[k * components], components);

            model.animChannels.push_back(std::move(ac));
        }
    }
}

void buildSceneDrawables(Model& model, cgltf_data* data,
                         const std::unordered_map<const cgltf_material*, int>& materialIndex,
                         std::string& outError) {
    // Map cgltf_node* to index
    std::unordered_map<const cgltf_node*, int> nodeToIdx;
    for (cgltf_size i = 0; i < data->nodes_count; ++i)
        nodeToIdx[&data->nodes[i]] = static_cast<int>(i);

    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (!scene) {
        outError = "glTF has no scene";
        return;
    }

    // Recursive lambda
    struct Context {
        Model& model;
        const std::unordered_map<const cgltf_node*, int>& nodeToIdx;
        const std::unordered_map<const cgltf_material*, int>& materialIndex;
        std::string& outError;
    };
    Context ctx{model, nodeToIdx, materialIndex, outError};

    std::function<void(const cgltf_node*, int, const std::array<double, 16>&)> traverse =
        [&](const cgltf_node* node, int nodeIdx, const std::array<double, 16>& parentWorld) {
            const std::array<double, 16> world = matMul(parentWorld, model.animNodes[nodeIdx].baseLocal);
            if (node->mesh) {
                for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                    if (!appendPrimitive(model, node->mesh->primitives[p], world, materialIndex, nodeIdx, ctx.outError))
                        return;
                }
            }
            for (cgltf_size c = 0; c < node->children_count; ++c) {
                auto it = nodeToIdx.find(node->children[c]);
                if (it != nodeToIdx.end())
                    traverse(node->children[c], it->second, world);
            }
        };

    for (cgltf_size n = 0; n < scene->nodes_count; ++n) {
        auto it = nodeToIdx.find(scene->nodes[n]);
        if (it != nodeToIdx.end())
            traverse(scene->nodes[n], it->second, identity());
    }
}

std::unique_ptr<Model> buildModel(cgltf_data* data, const std::string& basePath, std::string& outError) {
    auto model = std::make_unique<Model>();

    buildAnimNodes(*model, data);
    buildAnimChannels(*model, data);

    std::unordered_map<const cgltf_image*, int> textureCache;
    std::unordered_map<const cgltf_material*, int> materialIndex;
    model->materials.reserve(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& src = data->materials[i];
        Material mat;
        mat.doubleSided = src.double_sided;
        switch (src.alpha_mode) {
            case cgltf_alpha_mode_mask:
                mat.alphaMode = AlphaMode::Mask;
                mat.alphaCutoff = src.alpha_cutoff;
                break;
            case cgltf_alpha_mode_blend:
                mat.alphaMode = AlphaMode::Blend;
                break;
            default:
                mat.alphaMode = AlphaMode::Opaque;
                break;
        }
        if (src.has_pbr_metallic_roughness) {
            const auto& pbr = src.pbr_metallic_roughness;
            std::memcpy(mat.baseColorFactor.data(), pbr.base_color_factor, 4 * sizeof(float));
            if (pbr.base_color_texture.texture) {
                mat.baseColorTexture = decodeTexture(*model, textureCache, pbr.base_color_texture.texture->image, basePath);
            }
        }
        materialIndex.emplace(&src, static_cast<int>(model->materials.size()));
        model->materials.push_back(mat);
    }

    buildSceneDrawables(*model, data, materialIndex, outError);
    if (!outError.empty()) return nullptr;
    if (model->drawables.empty()) {
        outError = "glTF contains no triangle geometry";
        return nullptr;
    }

    return model;
}

} // namespace

std::unique_ptr<Model> loadModelFromFile(const std::string& path, std::string& outError) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    cgltf_result r = cgltf_parse_file(&options, path.c_str(), &data);
    if (r != cgltf_result_success) {
        outError = std::string("parse failed: ") + cgltfResultName(r);
        return nullptr;
    }
    r = cgltf_load_buffers(&options, data, path.c_str());
    if (r != cgltf_result_success) {
        outError = std::string("buffer load failed: ") + cgltfResultName(r);
        cgltf_free(data);
        return nullptr;
    }
    const size_t slash = path.find_last_of('/');
    const std::string basePath = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    auto model = buildModel(data, basePath, outError);
    cgltf_free(data);
    return model;
}

std::unique_ptr<Model> loadModelFromMemory(const uint8_t* bytes,
                                           size_t size,
                                           const std::string& basePath,
                                           std::string& outError) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    cgltf_result r = cgltf_parse(&options, bytes, size, &data);
    if (r != cgltf_result_success) {
        outError = std::string("parse failed: ") + cgltfResultName(r);
        return nullptr;
    }
    r = cgltf_load_buffers(&options, data, basePath.empty() ? nullptr : basePath.c_str());
    if (r != cgltf_result_success) {
        outError = std::string("buffer load failed: ") + cgltfResultName(r);
        cgltf_free(data);
        return nullptr;
    }
    auto model = buildModel(data, basePath, outError);
    cgltf_free(data);
    return model;
}

// --- Animation evaluation ---------------------------------------------------

namespace {

std::array<double, 4> quatSlerp(const std::array<double, 4>& a, const std::array<double, 4>& b, double t) {
    double dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    std::array<double, 4> bAdj = b;
    if (dot < 0) { dot = -dot; for (int i=0;i<4;++i) bAdj[i] = -bAdj[i]; }
    if (dot > 0.9995) {
        std::array<double,4> r;
        for (int i=0;i<4;++i) r[i] = a[i] + t*(bAdj[i]-a[i]);
        double len = std::sqrt(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]+r[3]*r[3]);
        if (len > 0) for (int i=0;i<4;++i) r[i] /= len;
        return r;
    }
    double theta = std::acos(dot);
    double sinTheta = std::sin(theta);
    double wa = std::sin((1-t)*theta) / sinTheta;
    double wb = std::sin(t*theta) / sinTheta;
    std::array<double,4> r;
    for (int i=0;i<4;++i) r[i] = wa*a[i] + wb*bAdj[i];
    return r;
}

// Compose translation * rotation (quaternion) * scale into a 4x4 matrix.
std::array<double, 16> composeTRS(const std::array<double, 3>& t,
                                   const std::array<double, 4>& q,
                                   const std::array<double, 3>& s) {
    double xx = q[0]*q[0], yy = q[1]*q[1], zz = q[2]*q[2];
    double xy = q[0]*q[1], xz = q[0]*q[2], xw = q[0]*q[3];
    double yz = q[1]*q[2], yw = q[1]*q[3], zw = q[2]*q[3];

    return {
        s[0]*(1-2*(yy+zz)),   s[0]*2*(xy+zw),       s[0]*2*(xz-yw),       0.0,
        s[1]*2*(xy-zw),       s[1]*(1-2*(xx+zz)),   s[1]*2*(yz+xw),       0.0,
        s[2]*2*(xz+yw),       s[2]*2*(yz-xw),       s[2]*(1-2*(xx+yy)),   0.0,
        t[0],                 t[1],                  t[2],                  1.0
    };
}

// Linearly interpolate between two keyframe values.
// For T/S (3 components) stride=3; for R (4 components) stride=4.
std::vector<float> lerpValues(const float* a, const float* b, double t, int stride) {
    std::vector<float> r(stride);
    for (int i = 0; i < stride; ++i)
        r[i] = static_cast<float>(a[i] + t * (b[i] - a[i]));
    return r;
}

// Find the keyframe bracket for a given time and compute the interpolation factor.
// Returns {idxA, idxB, t} where t is between 0 and 1.
struct Bracket { int a; int b; double t; };
Bracket findBracket(const std::vector<float>& times, double timeSec) {
    const size_t n = times.size();
    if (n == 0) return {0, 0, 0.0};
    if (timeSec <= times[0]) return {0, 0, 0.0};
    if (timeSec >= times[n-1]) return {static_cast<int>(n-1), static_cast<int>(n-1), 0.0};
    for (size_t i = 0; i < n - 1; ++i) {
        if (timeSec >= times[i] && timeSec < times[i+1]) {
            double t = (times[i+1] > times[i])
                ? (timeSec - times[i]) / (times[i+1] - times[i])
                : 0.0;
            return {static_cast<int>(i), static_cast<int>(i+1), t};
        }
    }
    return {0, 0, 0.0};
}

} // namespace

void evaluateAnimation(Model& model, double timeSec) {
    if (model.animChannels.empty() || model.animNodes.empty()) return;

    const size_t nodeCount = model.animNodes.size();

    // 1. Copy base TRS for all nodes (will be mutated by channels)
    std::vector<std::array<double, 3>> T(nodeCount);
    std::vector<std::array<double, 4>> R(nodeCount);
    std::vector<std::array<double, 3>> S(nodeCount);
    for (size_t i = 0; i < nodeCount; ++i) {
        T[i] = model.animNodes[i].baseT;
        R[i] = model.animNodes[i].baseR;
        S[i] = model.animNodes[i].baseS;
    }

    // 2. Apply all animation channels
    for (const auto& ch : model.animChannels) {
        const int ni = ch.nodeIndex;
        if (ni < 0 || ni >= static_cast<int>(nodeCount)) continue;

        auto [kA, kB, t] = findBracket(ch.times, timeSec);
        int stride = (ch.path == AnimChannel::Rotation) ? 4 : 3;
        const float* vA = ch.values.data() + kA * stride;
        const float* vB = ch.values.data() + kB * stride;

        if (ch.path == AnimChannel::Rotation) {
            std::array<double, 4> qA{vA[0], vA[1], vA[2], vA[3]};
            std::array<double, 4> qB{vB[0], vB[1], vB[2], vB[3]};
            R[ni] = quatSlerp(qA, qB, t);
        } else {
            auto lerped = lerpValues(vA, vB, t, 3);
            if (ch.path == AnimChannel::Translation) {
                T[ni] = {lerped[0], lerped[1], lerped[2]};
            } else { // Scale
                S[ni] = {lerped[0], lerped[1], lerped[2]};
            }
        }
    }

    // 3. Compute world transforms by walking hierarchy (parents before children)
    std::vector<std::array<double, 16>> worldTransforms(nodeCount);
    for (size_t i = 0; i < nodeCount; ++i) {
        auto local = composeTRS(T[i], R[i], S[i]);
        int parent = model.animNodes[i].parent;
        if (parent >= 0 && parent < static_cast<int>(i)) {
            // Parent already computed (we're iterating in increasing index order;
            // this assumes parent index < child index, which is usually true)
            worldTransforms[i] = matMul(worldTransforms[parent], local);
        } else {
            worldTransforms[i] = local;
        }
    }

    // 4. Update drawable transforms
    for (auto& d : model.drawables) {
        if (d.nodeIndex >= 0 && d.nodeIndex < static_cast<int>(nodeCount))
            d.transform = worldTransforms[d.nodeIndex];
    }
}

} // namespace maplibre_gltf

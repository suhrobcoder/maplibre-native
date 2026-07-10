#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace maplibre_gltf {

constexpr int kFloatsPerVertex = 8;
constexpr int kVertexStrideBytes = kFloatsPerVertex * sizeof(float);

struct Texture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

enum class AlphaMode : uint8_t {
    Opaque,
    Mask,
    Blend,
};

struct Material {
    std::array<float, 4> baseColorFactor = {1.f, 1.f, 1.f, 1.f};
    int baseColorTexture = -1;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

struct Drawable {
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    std::array<double, 16> transform;
    int material = -1;
    int nodeIndex = -1;
};

struct AnimChannel {
    int nodeIndex = -1;
    enum Path { Translation, Rotation, Scale } path;
    std::vector<float> times;
    std::vector<float> values;
};

struct AnimNode {
    int parent = -1;
    std::array<double, 16> baseLocal{};
    std::array<double, 3> baseT{};
    std::array<double, 4> baseR{0, 0, 0, 1};
    std::array<double, 3> baseS{1, 1, 1};
};

struct Model {
    std::vector<Drawable> drawables;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<AnimNode> animNodes;
    std::vector<AnimChannel> animChannels;

    bool hasAnimation() const { return !animChannels.empty(); }

    size_t totalVertices() const {
        size_t n = 0;
        for (const auto& d : drawables) n += d.vertices.size() / kFloatsPerVertex;
        return n;
    }
    size_t totalIndices() const {
        size_t n = 0;
        for (const auto& d : drawables) n += d.indices.size();
        return n;
    }
};

// Evaluate animation at timeSec. Fills each drawable's transform with the
// current world transform of its owning node.
void evaluateAnimation(Model& model, double timeSec);

std::unique_ptr<Model> loadModelFromFile(const std::string& path, std::string& outError);

std::unique_ptr<Model> loadModelFromMemory(const uint8_t* data,
                                           size_t size,
                                           const std::string& basePath,
                                           std::string& outError);

} // namespace maplibre_gltf

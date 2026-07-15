// Phase 5: Vulkan backend for the glTF model layer.
// Mirrors the GLES GltfMultiModelHost in interface and threading contract,
// but uses Vulkan instead of OpenGL ES 3.0.
//
// Push-constant layout (92 bytes total, fits within 128-byte limit):
//   mat4 matrix;      // offset 0,  size 64
//   vec4 baseColor;   // offset 64, size 16
//   float alphaCutoff;// offset 80, size 4
//   float hasTexture; // offset 84, size 4
//   float darkModeLighting; // offset 88, size 4
//
// Descriptor set 0: combined image sampler (binding 0) for the texture.
//
// Depth: viewport depth range set to [0, params.depthRangeSize] to match
// fill-extrusion's depth encoding.

#include "anchor_math.hpp"
#include "gltf_model.hpp"
#include "gltf_vulkan_shaders.h"

#include <android/log.h>
#include <jni.h>

#include <mbgl/style/layers/custom_layer_host.hpp>
#include <mbgl/style/layers/custom_layer_render_parameters.hpp>
#include <mbgl/style/layers/vulkan/custom_layer_init_parameters.hpp>
#include <mbgl/style/layers/vulkan/custom_layer_render_parameters.hpp>

#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace maplibre_gltf;

constexpr char LOG_TAG[] = "MapLibreGltfLayer";

// ---------------------------------------------------------------------------
// Push-constant structure (matches shader layout)
// ---------------------------------------------------------------------------
struct PushConstants {
    float matrix[16];   // offset 0, 64 bytes
    float baseColor[4]; // offset 64, 16 bytes
    float alphaCutoff;  // offset 80, 4 bytes
    float hasTexture;   // offset 84, 4 bytes — 1.0 if textured, 0.0 otherwise
    float darkModeLighting; // offset 88, 4 bytes — 1.0 for dark-mode lighting
};
static_assert(sizeof(PushConstants) <= 128, "Push constants exceed device limit");

// ---------------------------------------------------------------------------
// Convenience aliases for UniqueHandle with dynamic dispatcher
// ---------------------------------------------------------------------------
template <typename T>
using VkUniq = vk::UniqueHandle<T, vk::detail::DispatchLoaderDynamic>;

// ---------------------------------------------------------------------------
// Placement and LayerState (shared between JNI handle and render host)
// ---------------------------------------------------------------------------
struct Placement {
    double lat = 0, lng = 0;
    double scale = 1, bearingRad = 0, altitudeM = 0;
    double offsetEastM = 0, offsetNorthM = 0, offsetUpM = 0;
};

struct LayerState {
    struct Instance {
        std::shared_ptr<const Model> model;
        Placement placement;
    };
    std::mutex mutex;
    std::map<std::string, Instance> instances;
    bool darkModeLighting = false;
};

// ---------------------------------------------------------------------------
// 4x4 column-major helpers (single-precision for GPU upload)
// ---------------------------------------------------------------------------
inline std::array<float, 16> toFloat(const std::array<double, 16>& d) {
    std::array<float, 16> f;
    for (int i = 0; i < 16; ++i) f[i] = static_cast<float>(d[i]);
    return f;
}

// Transform a point by a column-major 4x4 in double (used for culling).
inline std::array<double, 4> mat4TransformPoint(const std::array<double, 16>& m, double x, double y, double z) {
    return {
        m[0] * x + m[4] * y + m[8] * z + m[12],
        m[1] * x + m[5] * y + m[9] * z + m[13],
        m[2] * x + m[6] * y + m[10] * z + m[14],
        m[3] * x + m[7] * y + m[11] * z + m[15],
    };
}

// ---------------------------------------------------------------------------
// Memory type helper
// ---------------------------------------------------------------------------
static uint32_t findMemoryType(vk::PhysicalDevice physDev,
                               uint32_t typeBits,
                               vk::MemoryPropertyFlags props,
                               const vk::detail::DispatchLoaderDynamic& d) {
    const auto memProps = physDev.getMemoryProperties(d);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Buffer creation helper (returns buffer + device memory)
// ---------------------------------------------------------------------------
static std::pair<VkUniq<vk::Buffer>, VkUniq<vk::DeviceMemory>> createBuffer(
    vk::Device device,
    vk::PhysicalDevice physDev,
    vk::DeviceSize size,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memProps,
    const vk::detail::DispatchLoaderDynamic& d) {
    const vk::BufferCreateInfo binfo{vk::BufferCreateFlags{}, size, usage, vk::SharingMode::eExclusive};
    auto buffer = device.createBufferUnique(binfo, nullptr, d);
    const auto reqs = device.getBufferMemoryRequirements(*buffer, d);
    const uint32_t memType = findMemoryType(physDev, reqs.memoryTypeBits, memProps, d);
    const vk::MemoryAllocateInfo ainfo{reqs.size, memType};
    auto memory = device.allocateMemoryUnique(ainfo, nullptr, d);
    device.bindBufferMemory(*buffer, *memory, 0, d);
    return {std::move(buffer), std::move(memory)};
}

// ---------------------------------------------------------------------------
// Main Vulkan host class
// ---------------------------------------------------------------------------
class GltfVulkanMultiModelHost final : public mbgl::style::CustomLayerHost {
public:
    explicit GltfVulkanMultiModelHost(std::shared_ptr<LayerState> state_)
        : state(std::move(state_)) {}

    // -----------------------------------------------------------------------
    // initialize() — create shader modules, pipeline layout, descriptor pool
    // -----------------------------------------------------------------------
    void initialize(const mbgl::style::CustomLayerInitParameters& baseParams) override {
        const auto& params = static_cast<const mbgl::style::vulkan::CustomLayerInitParameters&>(baseParams);
        dev = params.device;
        physDev = params.physicalDevice;
        disp = &params.dispatcher;

        // Create shader modules from embedded SPIR-V
        {
            const vk::ShaderModuleCreateInfo vci{{}, sizeof(kVertSpirv), kVertSpirv};
            auto r = dev.createShaderModuleUnique(vci, nullptr, *disp);
            vertModule = std::move(r);
        }
        {
            const vk::ShaderModuleCreateInfo fci{{}, sizeof(kFragSpirv), kFragSpirv};
            auto r = dev.createShaderModuleUnique(fci, nullptr, *disp);
            fragModule = std::move(r);
        }

        // Descriptor set layout: binding 0 = combined image sampler (fragment)
        const vk::DescriptorSetLayoutBinding dslb{
            0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment};
        const vk::DescriptorSetLayoutCreateInfo dslci{{}, 1, &dslb};
        descSetLayout = dev.createDescriptorSetLayoutUnique(dslci, nullptr, *disp);

        // Pipeline layout: descriptor set + push constants
        const vk::PushConstantRange pcr{
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(PushConstants)};
        const vk::PipelineLayoutCreateInfo plci{{}, 1, &*descSetLayout, 1, &pcr};
        pipelineLayout = dev.createPipelineLayoutUnique(plci, nullptr, *disp);

        // Descriptor pool: one combined image sampler
        const vk::DescriptorPoolSize dps{vk::DescriptorType::eCombinedImageSampler, kMaxTextures};
        const vk::DescriptorPoolCreateInfo dpci{{}, kMaxTextures, 1, &dps};
        descPool = dev.createDescriptorPoolUnique(dpci, nullptr, *disp);

        __android_log_print(
            ANDROID_LOG_INFO, LOG_TAG, "Vulkan host initialized: device=%p", static_cast<VkDevice>(dev));
    }

    // -----------------------------------------------------------------------
    // render() — main draw loop, called once per frame
    // -----------------------------------------------------------------------
    void render(const mbgl::style::CustomLayerRenderParameters& baseParams) override {
        const auto& params = static_cast<const mbgl::style::vulkan::CustomLayerRenderParameters&>(baseParams);
        dev = params.device;
        disp = &params.dispatcher;
        cmd = params.commandBuffer;
        const float preRot = params.screenPreRotationRadiansClockwise;

        // Snapshot instances under the lock
        struct DrawItem {
            std::shared_ptr<const Model> model;
            Placement placement;
        };
        std::vector<DrawItem> items;
        bool darkModeLighting = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            darkModeLighting = state->darkModeLighting;
            items.reserve(state->instances.size());
            for (const auto& [id, inst] : state->instances) {
                items.push_back({inst.model, inst.placement});
            }
        }

        __android_log_print(ANDROID_LOG_INFO,
                            LOG_TAG,
                            "render: %zu instances, w=%.0f h=%.0f depth=%.2f",
                            items.size(),
                            params.width,
                            params.height,
                            params.depthRangeSize);

        // Upload new models; free orphaned GPU copies
        for (const auto& item : items) {
            if (gpu.find(item.model.get()) == gpu.end()) {
                uploadModel(item.model);
            }
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

        // Lazy pipeline creation (need render pass from first frame)
        if (!pipeline) {
            createPipeline(params.renderPass);
        }
        if (!pipeline) return;

        // Set viewport depth range to match fill-extrusion encoding
        const float depthRange = static_cast<float>(params.depthRangeSize);
        const vk::Viewport vp{
            0.0f, 0.0f, static_cast<float>(params.width), static_cast<float>(params.height), 0.0f, depthRange};
        const vk::Rect2D scissor{{0, 0}, {static_cast<uint32_t>(params.width), static_cast<uint32_t>(params.height)}};
        cmd.setViewport(0, {vp}, *disp);
        cmd.setScissor(0, {scissor}, *disp);

        // Bind pipeline
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline, *disp);
        __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "pipeline bound, drawing %zu items", items.size());

        // Opaque drawables first, then blended
        struct BlendCall {
            const GpuDrawable* gd;
            const Model* model;
            std::array<double, 16> base;
            double depth;
        };
        std::vector<BlendCall> blendCalls;

        for (const auto& item : items) {
            const GpuModel& gm = gpu.at(item.model.get());

            // Anchor-relative MVP
            const std::array<double, 16> anchorMvp = anchorMatrix(
                params.nearClippedProjectionMatrix, item.placement.lat, item.placement.lng, params.zoom);

            // glTF axis swap + placement transform (same math as GLES host)
            const double s = item.placement.scale;
            const double r = item.placement.bearingRad;
            const double cr = std::cos(r), sr = std::sin(r);
            const double offsetEast = item.placement.offsetEastM * cr + item.placement.offsetNorthM * sr;
            const double offsetSouth = item.placement.offsetEastM * sr - item.placement.offsetNorthM * cr;
            const std::array<double, 16> placement = {
                s * cr,
                s * sr,
                0.0,
                0.0,
                0.0,
                0.0,
                s,
                0.0,
                -s * sr,
                s * cr,
                0.0,
                0.0,
                offsetEast,
                offsetSouth,
                item.placement.altitudeM + item.placement.offsetUpM,
                1.0,
            };

            std::array<double, 16> base;
            mat4Multiply(base, anchorMvp, placement);

            if (isCulled(base, gm)) {
                __android_log_print(ANDROID_LOG_DEBUG,
                                    LOG_TAG,
                                    "FRUSTUM CULLED: lat=%.4f lng=%.4f",
                                    item.placement.lat,
                                    item.placement.lng);
                continue;
            }

            for (const auto& gd : gm.drawables) {
                const Material& mat = gd.material >= 0 ? item.model->materials[gd.material] : kDefaultMaterial;
                if (mat.alphaMode == AlphaMode::Blend) {
                    const auto c = mat4TransformPoint(base, gd.center[0], gd.center[1], gd.center[2]);
                    const double w = c[3] != 0.0 ? c[3] : 1.0;
                    blendCalls.push_back({&gd, item.model.get(), base, c[2] / w});
                    continue;
                }
                drawOne(gd, mat, base, preRot, darkModeLighting);
            }
        }

        // Blended pass: sort back-to-front, draw with blending
        if (!blendCalls.empty()) {
            std::sort(blendCalls.begin(), blendCalls.end(), [](const BlendCall& a, const BlendCall& b) {
                return a.depth > b.depth;
            });
            for (const auto& bc : blendCalls) {
                const Material& mat = bc.gd->material >= 0 ? bc.model->materials[bc.gd->material] : kDefaultMaterial;
                drawOne(*bc.gd, mat, bc.base, preRot, darkModeLighting);
            }
        }
    }

    void contextLost() override {
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, "Vulkan contextLost");
        releaseAll();
    }

    void deinitialize() override {
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, "Vulkan deinitialize");
        releaseAll();
    }

private:
    // -----------------------------------------------------------------------
    // GpuDrawable / GpuModel (Vulkan GPU resources)
    // -----------------------------------------------------------------------
    struct GpuDrawable {
        VkUniq<vk::Buffer> vertexBuffer;
        VkUniq<vk::DeviceMemory> vertexMemory;
        VkUniq<vk::Buffer> indexBuffer;
        VkUniq<vk::DeviceMemory> indexMemory;
        uint32_t indexCount = 0;

        // Texture handles (nullopt if untextured)
        VkUniq<vk::Image> textureImage;
        VkUniq<vk::DeviceMemory> textureMemory;
        VkUniq<vk::ImageView> textureView;
        VkUniq<vk::Sampler> sampler;
        VkUniq<vk::DescriptorSet> descriptorSet;

        int material = -1;
        std::array<double, 16> transform;
        std::array<double, 3> center{0, 0, 0};
    };

    struct GpuModel {
        std::shared_ptr<const Model> model;
        std::vector<GpuDrawable> drawables;
        std::array<double, 3> bboxMin{0, 0, 0}, bboxMax{0, 0, 0};
    };

    static constexpr int kMaxTextures = 256;

    // -----------------------------------------------------------------------
    // Upload a model to GPU
    // -----------------------------------------------------------------------
    void uploadModel(const std::shared_ptr<const Model>& model) {
        if (!dev || !disp) return;

        GpuModel gm;
        gm.model = model;

        bool bboxInit = false;
        gm.drawables.reserve(model->drawables.size());

        for (const Drawable& d : model->drawables) {
            GpuDrawable gd;
            gd.transform = d.transform;
            gd.material = d.material;
            gd.indexCount = static_cast<uint32_t>(d.indices.size());

            // Vertex buffer
            const vk::DeviceSize vertBytes = d.vertices.size() * sizeof(float);
            auto [vb, vbMem] = createBuffer(
                dev,
                physDev,
                vertBytes,
                vk::BufferUsageFlagBits::eVertexBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                *disp);
            gd.vertexBuffer = std::move(vb);
            gd.vertexMemory = std::move(vbMem);
            {
                auto* mapped = dev.mapMemory(*gd.vertexMemory, 0, vertBytes, {}, *disp);
                std::memcpy(mapped, d.vertices.data(), static_cast<size_t>(vertBytes));
                dev.unmapMemory(*gd.vertexMemory, *disp);
            }

            // Index buffer
            const vk::DeviceSize idxBytes = d.indices.size() * sizeof(uint32_t);
            auto [ib, ibMem] = createBuffer(
                dev,
                physDev,
                idxBytes,
                vk::BufferUsageFlagBits::eIndexBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                *disp);
            gd.indexBuffer = std::move(ib);
            gd.indexMemory = std::move(ibMem);
            {
                auto* mapped = dev.mapMemory(*gd.indexMemory, 0, idxBytes, {}, *disp);
                std::memcpy(mapped, d.indices.data(), static_cast<size_t>(idxBytes));
                dev.unmapMemory(*gd.indexMemory, *disp);
            }

            // Texture
            if (d.material >= 0) {
                const int texIdx = model->materials[d.material].baseColorTexture;
                if (texIdx >= 0 && texIdx < static_cast<int>(model->textures.size())) {
                    createTexture(gd, model->textures[texIdx]);
                }
            }

            // AABB computation (same as GLES host)
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

            gm.drawables.push_back(std::move(gd));
        }

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Vulkan uploaded model: %zu drawables", gm.drawables.size());
        gpu.emplace(model.get(), std::move(gm));
    }

    // -----------------------------------------------------------------------
    // Create texture resources for a GpuDrawable
    // -----------------------------------------------------------------------
    void createTexture(GpuDrawable& gd, const Texture& tex) {
        if (!dev || !disp) return;

        // Create image
        const vk::ImageCreateInfo ici{{},
                                      vk::ImageType::e2D,
                                      vk::Format::eR8G8B8A8Unorm,
                                      {static_cast<uint32_t>(tex.width), static_cast<uint32_t>(tex.height), 1},
                                      1,
                                      1,
                                      vk::SampleCountFlagBits::e1,
                                      vk::ImageTiling::eLinear,
                                      vk::ImageUsageFlagBits::eSampled,
                                      vk::SharingMode::eExclusive};
        auto img = dev.createImageUnique(ici, nullptr, *disp);
        const auto imgReqs = dev.getImageMemoryRequirements(*img, *disp);
        const uint32_t memType = findMemoryType(
            physDev,
            imgReqs.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            *disp);
        vk::MemoryAllocateInfo mai{imgReqs.size, memType};
        auto imgMem = dev.allocateMemoryUnique(mai, nullptr, *disp);
        dev.bindImageMemory(*img, *imgMem, 0, *disp);

        // Upload pixels (linear tiling — directly writable)
        auto* mapped = dev.mapMemory(*imgMem, 0, imgReqs.size, {}, *disp);
        const vk::SubresourceLayout layout = dev.getImageSubresourceLayout(
            *img, {vk::ImageAspectFlagBits::eColor, 0, 0}, *disp);
        const size_t rowBytes = static_cast<size_t>(tex.width) * 4;
        const size_t srcRowBytes = rowBytes;
        for (int y = 0; y < tex.height; ++y) {
            std::memcpy(static_cast<uint8_t*>(mapped) + layout.offset + y * layout.rowPitch,
                        tex.rgba.data() + y * srcRowBytes,
                        srcRowBytes);
        }
        dev.unmapMemory(*imgMem, *disp);

        // Image view
        const vk::ImageViewCreateInfo ivi{{},
                                          *img,
                                          vk::ImageViewType::e2D,
                                          vk::Format::eR8G8B8A8Unorm,
                                          {},
                                          {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        auto view = dev.createImageViewUnique(ivi, nullptr, *disp);

        // Sampler (bilinear, clamp to edge — matches GLES)
        const vk::SamplerCreateInfo sci{{},
                                        vk::Filter::eLinear,
                                        vk::Filter::eLinear,
                                        vk::SamplerMipmapMode::eNearest,
                                        vk::SamplerAddressMode::eClampToEdge,
                                        vk::SamplerAddressMode::eClampToEdge,
                                        vk::SamplerAddressMode::eClampToEdge};
        auto sampler = dev.createSamplerUnique(sci, nullptr, *disp);

        // Descriptor set
        auto descSets = dev.allocateDescriptorSetsUnique(vk::DescriptorSetAllocateInfo{*descPool, 1, &*descSetLayout},
                                                         *disp);
        auto descSet = std::move(descSets[0]);

        const vk::DescriptorImageInfo dii{*sampler, *view, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::WriteDescriptorSet wds{*descSet, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &dii};
        dev.updateDescriptorSets({wds}, {}, *disp);

        gd.textureImage = std::move(img);
        gd.textureMemory = std::move(imgMem);
        gd.textureView = std::move(view);
        gd.sampler = std::move(sampler);
        gd.descriptorSet = std::move(descSet);
    }

    // -----------------------------------------------------------------------
    // Create Vulkan pipeline (lazy, on first render)
    // -----------------------------------------------------------------------
    void createPipeline(vk::RenderPass renderPass) {
        if (!dev || !disp || !vertModule || !fragModule || !pipelineLayout) return;

        const vk::PipelineShaderStageCreateInfo stages[] = {
            {{}, vk::ShaderStageFlagBits::eVertex, *vertModule, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fragModule, "main"},
        };

        // Vertex input: interleaved position(3) + normal(3) + uv(2) = 8 floats
        const vk::VertexInputBindingDescription bindings[] = {{0, kVertexStrideBytes, vk::VertexInputRate::eVertex}};
        const vk::VertexInputAttributeDescription attribs[] = {
            {0, 0, vk::Format::eR32G32B32Sfloat, 0},
            {1, 0, vk::Format::eR32G32B32Sfloat, 3 * sizeof(float)},
            {2, 0, vk::Format::eR32G32Sfloat, 6 * sizeof(float)},
        };
        const vk::PipelineVertexInputStateCreateInfo vertexInput{{}, 1, bindings, 3, attribs};

        const vk::PipelineInputAssemblyStateCreateInfo inputAssembly{{}, vk::PrimitiveTopology::eTriangleList};

        const vk::PipelineViewportStateCreateInfo viewportState{{}, 1, nullptr, 1, nullptr};

        const vk::PipelineRasterizationStateCreateInfo rasterizer{{},
                                                                  VK_FALSE,
                                                                  VK_FALSE,
                                                                  vk::PolygonMode::eFill,
                                                                  vk::CullModeFlagBits::eBack,
                                                                  vk::FrontFace::eCounterClockwise,
                                                                  VK_FALSE,
                                                                  0.0f,
                                                                  0.0f,
                                                                  0.0f,
                                                                  1.0f};

        const vk::PipelineMultisampleStateCreateInfo multisampling{};

        // Depth test ON, LEQUAL, writes ON (matches GLES: depth mask true, LEQUAL)
        const vk::PipelineDepthStencilStateCreateInfo depthStencil{
            {}, VK_TRUE, VK_TRUE, vk::CompareOp::eLessOrEqual, VK_FALSE, VK_FALSE};

        // No blend for opaque pass; blend is toggled per drawable by
        // VK_DYNAMIC_STATE_BLEND_CONSTANTS ... actually, we can just always
        // enable blend (it's a no-op when src alpha = 1.0). But to be clean,
        // set no blend here; we'll adjust for the blend pass via dynamic state.

        // Actually for simplicity, enable blend with ONE/ONE_MINUS_SRC_ALPHA
        // always. Opaque drawables write alpha = 1, so blend is a no-op.
        const vk::PipelineColorBlendAttachmentState blendAtt{
            VK_TRUE,
            vk::BlendFactor::eSrcAlpha,
            vk::BlendFactor::eOneMinusSrcAlpha,
            vk::BlendOp::eAdd,
            vk::BlendFactor::eOne,
            vk::BlendFactor::eZero,
            vk::BlendOp::eAdd,
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB |
                vk::ColorComponentFlagBits::eA,
        };
        const vk::PipelineColorBlendStateCreateInfo colorBlend{{}, VK_FALSE, vk::LogicOp::eCopy, 1, &blendAtt};

        const vk::DynamicState dynStates[] = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };
        const vk::PipelineDynamicStateCreateInfo dynamicState{{}, 2, dynStates};

        const vk::GraphicsPipelineCreateInfo pci{{},
                                                 2,
                                                 stages,
                                                 &vertexInput,
                                                 &inputAssembly,
                                                 nullptr,
                                                 &viewportState,
                                                 &rasterizer,
                                                 &multisampling,
                                                 &depthStencil,
                                                 &colorBlend,
                                                 &dynamicState,
                                                 *pipelineLayout,
                                                 renderPass};

        auto result = dev.createGraphicsPipelineUnique(nullptr, pci, nullptr, *disp);
        if (result.result != vk::Result::eSuccess) {
            __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Vulkan pipeline creation failed");
            return;
        }
        pipeline = std::move(result.value);
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, "Vulkan pipeline created");
    }

    // -----------------------------------------------------------------------
    // Issue one indexed draw call
    // -----------------------------------------------------------------------
    void drawOne(const GpuDrawable& gd,
                 const Material& mat,
                 const std::array<double, 16>& base,
                 float preRot,
                 bool darkModeLighting) {
        if (!cmd || !dev || !disp) return;
        __android_log_print(ANDROID_LOG_DEBUG,
                            LOG_TAG,
                            "drawOne: idxCount=%u mat=%d tex=%d",
                            gd.indexCount,
                            gd.material,
                            gd.descriptorSet ? 1 : 0);
        // Compose MVP = base * node transform
        std::array<double, 16> mvpD;
        mat4Multiply(mvpD, base, gd.transform);

        // Apply screen pre-rotation (needed on some Android devices)
        if (preRot != 0.0f) {
            // Pre-multiply by Z-rotation matrix: MVP = rotZ * MVP
            const float cr = std::cos(preRot);
            const float sr = std::sin(preRot);
            const std::array<double, 16> rotZ = {
                cr,
                sr,
                0.0,
                0.0,
                -sr,
                cr,
                0.0,
                0.0,
                0.0,
                0.0,
                1.0,
                0.0,
                0.0,
                0.0,
                0.0,
                1.0,
            };
            std::array<double, 16> tmp;
            mat4Multiply(tmp, rotZ, mvpD);
            mvpD = tmp;
        }

        // Fill push constants
        PushConstants pc;
        const auto mvpF = toFloat(mvpD);
        std::memcpy(pc.matrix, mvpF.data(), sizeof(pc.matrix));
        std::memcpy(pc.baseColor, mat.baseColorFactor.data(), sizeof(pc.baseColor));
        pc.alphaCutoff = (mat.alphaMode == AlphaMode::Mask) ? mat.alphaCutoff : 0.0f;
        pc.hasTexture = (gd.descriptorSet) ? 1.0f : 0.0f;
        pc.darkModeLighting = darkModeLighting ? 1.0f : 0.0f;

        cmd.pushConstants(*pipelineLayout,
                          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                          0,
                          sizeof(PushConstants),
                          &pc,
                          *disp);

        // Bind descriptor set (texture) if present
        if (gd.descriptorSet) {
            cmd.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics, *pipelineLayout, 0, {*gd.descriptorSet}, {}, *disp);
        }

        // Bind buffers
        cmd.bindVertexBuffers(0, {*gd.vertexBuffer}, {0}, *disp);
        cmd.bindIndexBuffer(*gd.indexBuffer, 0, vk::IndexType::eUint32, *disp);

        // Draw
        cmd.drawIndexed(gd.indexCount, 1, 0, 0, 0, *disp);
    }

    // -----------------------------------------------------------------------
    // Frustum culling (same as GLES host)
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Release all GPU resources
    // -----------------------------------------------------------------------
    void releaseGpuModel(GpuModel& gm) { gm.drawables.clear(); }

    void releaseAll() {
        gpu.clear();
        pipeline.reset();
        pipelineLayout.reset();
        descSetLayout.reset();
        descPool.reset();
        vertModule.reset();
        fragModule.reset();
    }

    // -----------------------------------------------------------------------
    // Default material (opaque white)
    // -----------------------------------------------------------------------
    static inline const Material kDefaultMaterial{};

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    std::shared_ptr<LayerState> state;

    // Device handles (copied from params, which may be destroyed after render())
    vk::Device dev;
    vk::PhysicalDevice physDev;
    const vk::detail::DispatchLoaderDynamic* disp = nullptr;
    vk::CommandBuffer cmd;

    // Pipeline resources
    VkUniq<vk::ShaderModule> vertModule;
    VkUniq<vk::ShaderModule> fragModule;
    VkUniq<vk::DescriptorSetLayout> descSetLayout;
    VkUniq<vk::PipelineLayout> pipelineLayout;
    VkUniq<vk::Pipeline> pipeline;
    VkUniq<vk::DescriptorPool> descPool;

    // Per-model GPU resources
    std::map<const Model*, GpuModel> gpu;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// JNI entry points for org.maplibre.gltf.GltfModelLayer
// Identical signatures to the GLES version.
// ---------------------------------------------------------------------------

namespace {
inline LayerState* stateOf(jlong handle) {
    return reinterpret_cast<std::shared_ptr<LayerState>*>(handle)->get();
}
inline Placement placementOf(jdouble lat,
                             jdouble lng,
                             jdouble scale,
                             jdouble bearingRad,
                             jdouble altitudeM,
                             jdouble offsetEastM,
                             jdouble offsetNorthM,
                             jdouble offsetUpM) {
    return Placement{lat, lng, scale, bearingRad, altitudeM, offsetEastM, offsetNorthM, offsetUpM};
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
        new GltfVulkanMultiModelHost(*reinterpret_cast<std::shared_ptr<LayerState>*>(stateHandle)));
}

extern "C" JNIEXPORT void JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeSetDarkModeLighting(JNIEnv*,
                                                                                                    jclass,
                                                                                                    jlong stateHandle,
                                                                                                    jboolean enabled) {
    LayerState* state = stateOf(stateHandle);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->darkModeLighting = enabled == JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeAddModel(JNIEnv* env,
                                                                                           jclass,
                                                                                           jlong stateHandle,
                                                                                           jstring jid,
                                                                                           jlong modelHandle,
                                                                                           jdouble lat,
                                                                                           jdouble lng,
                                                                                           jdouble scale,
                                                                                           jdouble bearingRad,
                                                                                           jdouble altitudeM,
                                                                                           jdouble offsetEastM,
                                                                                           jdouble offsetNorthM,
                                                                                           jdouble offsetUpM,
                                                                                           jfloat,
                                                                                           jdouble) {
    std::shared_ptr<const Model> model(reinterpret_cast<Model*>(modelHandle));
    const char* idChars = env->GetStringUTFChars(jid, nullptr);
    std::string id(idChars);
    env->ReleaseStringUTFChars(jid, idChars);

    LayerState* state = stateOf(stateHandle);
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto [it, inserted] = state->instances.emplace(
        std::move(id),
        LayerState::Instance{
            std::move(model), placementOf(lat, lng, scale, bearingRad, altitudeM, offsetEastM, offsetNorthM, offsetUpM)});
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
                                                                                              jdouble bearingRad,
                                                                                              jdouble altitudeM,
                                                                                              jdouble offsetEastM,
                                                                                              jdouble offsetNorthM,
                                                                                              jdouble offsetUpM,
                                                                                              jfloat,
                                                                                              jdouble) {
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
        LayerState::Instance{
            src->second.model, placementOf(lat, lng, scale, bearingRad, altitudeM, offsetEastM, offsetNorthM, offsetUpM)});
    return inserted ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_maplibre_gltf_GltfModelLayer_nativeUpdateModel(JNIEnv* env,
                                                                                              jclass,
                                                                                              jlong stateHandle,
                                                                                              jstring jid,
                                                                                              jdouble lat,
                                                                                              jdouble lng,
                                                                                              jdouble scale,
                                                                                              jdouble bearingRad,
                                                                                              jdouble altitudeM,
                                                                                              jdouble offsetEastM,
                                                                                              jdouble offsetNorthM,
                                                                                              jdouble offsetUpM,
                                                                                              jfloat,
                                                                                              jdouble) {
    const char* idChars = env->GetStringUTFChars(jid, nullptr);
    std::string id(idChars);
    env->ReleaseStringUTFChars(jid, idChars);

    LayerState* state = stateOf(stateHandle);
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->instances.find(id);
    if (it == state->instances.end()) return JNI_FALSE;
    it->second.placement = placementOf(lat, lng, scale, bearingRad, altitudeM, offsetEastM, offsetNorthM, offsetUpM);
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

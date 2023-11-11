#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <fstream>
#include <filesystem>

#include "recomp_ui.h"

#include "concurrentqueue.h"

#include "rt64_layer.h"
#include "rt64_render_hooks.h"
#include "rt64_render_interface_builders.h"

#include "RmlUi/Core.h"
#include "RmlUi/Debugger.h"
#include "RmlUi_Platform_SDL.h"

#include "InterfaceVS.hlsl.spirv.h"
#include "InterfacePS.hlsl.spirv.h"

#ifdef _WIN32
#   include "InterfaceVS.hlsl.dxil.h"
#   include "InterfacePS.hlsl.dxil.h"
#endif

#ifdef _WIN32
#    define GET_SHADER_BLOB(name, format) \
        ((format) == RT64::RenderShaderFormat::SPIRV ? name##BlobSPIRV : \
        (format) == RT64::RenderShaderFormat::DXIL ? name##BlobDXIL : nullptr)
#    define GET_SHADER_SIZE(name, format) \
        ((format) == RT64::RenderShaderFormat::SPIRV ? std::size(name##BlobSPIRV) : \
        (format) == RT64::RenderShaderFormat::DXIL ? std::size(name##BlobDXIL) : 0)
#else
#    define GET_SHADER_BLOB(name, format) \
        ((format) == RT64::RenderShaderFormat::SPIRV ? name##BlobSPIRV : nullptr)
#    define GET_SHADER_SIZE(name, format) \
        ((format) == RT64::RenderShaderFormat::SPIRV ? std::size(name##BlobSPIRV) : 0)
#endif

struct UIRenderContext {
    RT64::RenderInterface* interface;
    RT64::RenderDevice* device;
    Rml::ElementDocument* document;
};

// TODO deduplicate from rt64_common.h
void CalculateTextureRowWidthPadding(uint32_t rowPitch, uint32_t &rowWidth, uint32_t &rowPadding) {
    const int RowMultiple = 256;
    rowWidth = rowPitch;
    rowPadding = (rowWidth % RowMultiple) ? RowMultiple - (rowWidth % RowMultiple) : 0;
    rowWidth += rowPadding;
}

struct RmlRenderInterfaceHeapBase : public RT64::RenderDescriptorHeapBase {
    uint32_t gSampler;
    uint32_t gTexture;

    RmlRenderInterfaceHeapBase(const RT64::RenderSampler* linear_sampler) {
        assert(linear_sampler != nullptr);

        builder.begin();
        builder.beginSet();
        gSampler = builder.addImmutableSampler(1, linear_sampler);
        gTexture = builder.addTexture(2);
        builder.endSet();
        builder.end();
    }
};

struct RmlPushConstants {
    Rml::Matrix4f transform;
    Rml::Vector2f translation;
};

struct TextureHandle {
    std::unique_ptr<RT64::RenderTexture> texture;
    std::unique_ptr<RT64::RenderDescriptorHeap> heap;
};

std::vector<char> read_file(const std::filesystem::path& filepath) {
    std::vector<char> ret{};
    std::ifstream input_file{ filepath, std::ios::binary };

    if (!input_file) {
        return ret;
    }

    input_file.seekg(0, std::ios::end);
    std::streampos filesize = input_file.tellg();
    input_file.seekg(0, std::ios::beg);

    ret.resize(filesize);

    input_file.read(ret.data(), filesize);

    return ret;
}


template <typename T>
T from_bytes_le(const char* input) {
    return *reinterpret_cast<const T*>(input);
}

void load_document();

class RmlRenderInterface_RT64 : public Rml::RenderInterface {
    static constexpr uint32_t per_frame_descriptor_set = 0;
    static constexpr uint32_t per_draw_descriptor_set = 1;

    static constexpr uint32_t initial_upload_buffer_size = 1024 * 1024;
    static constexpr uint32_t initial_vertex_buffer_size = 512 * sizeof(Rml::Vertex);
    static constexpr uint32_t initial_index_buffer_size = 1024 * sizeof(int);
    static constexpr RT64::RenderFormat RmlTextureFormat = RT64::RenderFormat::B8G8R8A8_UNORM;
    static constexpr uint32_t RmlTextureFormatBytesPerPixel = RenderFormatSize(RmlTextureFormat);
    struct UIRenderContext* render_context_;
    int scissor_x_ = 0;
    int scissor_y_ = 0;
    int scissor_width_ = 0;
    int scissor_height_ = 0;
    int window_width_ = 0;
    int window_height_ = 0;
    Rml::Matrix4f projection_mtx_ = Rml::Matrix4f::Identity();
    Rml::Matrix4f transform_ = Rml::Matrix4f::Identity();
    Rml::Matrix4f mvp_ = Rml::Matrix4f::Identity();
    std::unordered_map<Rml::TextureHandle, TextureHandle> textures_{};
    Rml::TextureHandle texture_count_ = 1; // Start at 1 to reserve texture 0 as the 1x1 pixel white texture
    std::unique_ptr<RT64::RenderBuffer> upload_buffer_{};
    std::unique_ptr<RT64::RenderBuffer> vertex_buffer_{};
    std::unique_ptr<RT64::RenderBuffer> index_buffer_{};
    std::unique_ptr<RT64::RenderSampler> nearestSampler_{};
    std::unique_ptr<RT64::RenderSampler> linearSampler_{};
    std::unique_ptr<RT64::RenderShader> vertex_shader_{};
    std::unique_ptr<RT64::RenderShader> pixel_shader_{};
    std::unique_ptr<RmlRenderInterfaceHeapBase> heap_base_{};
    std::unique_ptr<RT64::RenderPipelineLayout> layout_{};
    std::unique_ptr<RT64::RenderPipeline> pipeline_{};
    uint32_t upload_buffer_size_ = 0;
    uint32_t upload_buffer_bytes_used_ = 0;
    uint8_t* upload_buffer_mapped_data_ = nullptr;
    uint32_t vertex_buffer_size_ = 0;
    uint32_t index_buffer_size_ = 0;
    RT64::RenderInputSlot vertex_slot_{ 0, sizeof(Rml::Vertex) };
    RT64::RenderCommandList* list_ = nullptr;
    bool scissor_enabled_ = false;
    std::vector<std::unique_ptr<RT64::RenderBuffer>> stale_buffers_{};
public:
    RmlRenderInterface_RT64(struct UIRenderContext* render_context) {
        render_context_ = render_context;

        // Create the texture upload buffer, vertex buffer and index buffer
        resize_upload_buffer(initial_upload_buffer_size, false);
        resize_vertex_buffer(initial_vertex_buffer_size);
        resize_index_buffer(initial_index_buffer_size);

        // Describe the vertex format
        std::vector<RT64::RenderInputElement> vertex_elements{};
        vertex_elements.emplace_back(RT64::RenderInputElement{ "POSITION", 0, 0, RT64::RenderFormat::R32G32_FLOAT, 0,   offsetof(Rml::Vertex, position) });
        vertex_elements.emplace_back(RT64::RenderInputElement{ "COLOR",    0, 1, RT64::RenderFormat::R8G8B8A8_UNORM, 0, offsetof(Rml::Vertex, colour) });
        vertex_elements.emplace_back(RT64::RenderInputElement{ "TEXCOORD", 0, 2, RT64::RenderFormat::R32G32_FLOAT, 0,   offsetof(Rml::Vertex, tex_coord) });

        // Create a nearest sampler and a linear sampler
        RT64::RenderSamplerDesc samplerDesc;
        samplerDesc.minFilter = RT64::RenderFilter::NEAREST;
        samplerDesc.magFilter = RT64::RenderFilter::NEAREST;
        samplerDesc.addressU = RT64::RenderTextureAddressMode::CLAMP;
        samplerDesc.addressV = RT64::RenderTextureAddressMode::CLAMP;
        samplerDesc.addressW = RT64::RenderTextureAddressMode::CLAMP;
        nearestSampler_ = render_context->device->createSampler(samplerDesc);

        samplerDesc.minFilter = RT64::RenderFilter::LINEAR;
        samplerDesc.magFilter = RT64::RenderFilter::LINEAR;
        linearSampler_ = render_context->device->createSampler(samplerDesc);

        // Create the shaders
        RT64::RenderShaderFormat shaderFormat = render_context->interface->getCapabilities().shaderFormat;

        vertex_shader_ = render_context->device->createShader(GET_SHADER_BLOB(InterfaceVS, shaderFormat), GET_SHADER_SIZE(InterfaceVS, shaderFormat), "VSMain", shaderFormat);
        pixel_shader_ = render_context->device->createShader(GET_SHADER_BLOB(InterfacePS, shaderFormat), GET_SHADER_SIZE(InterfacePS, shaderFormat), "PSMain", shaderFormat);


        // Create the descriptor heap
        heap_base_ = std::make_unique<RmlRenderInterfaceHeapBase>(linearSampler_.get());

        // Create the pipeline layout
        RT64::RenderPipelineLayoutBuilder layout_builder{};
        layout_builder.begin(false, true);
        layout_builder.addPushConstant(0, 0, sizeof(RmlPushConstants), RT64::RenderShaderStageFlag::VERTEX);
        // Add the descriptor set for descriptors changed once per frame.
        layout_builder.addDescriptorSetsFromHeap(heap_base_->builder);
        layout_builder.end();
        layout_ = layout_builder.create(render_context->device);

        // Create the pipeline description
        RT64::RenderGraphicsPipelineDesc pipeline_desc{};
        pipeline_desc.renderTargetBlend[0] = RT64::RenderBlendDesc::AlphaBlend();
        pipeline_desc.renderTargetFormat[0] = RT64::RenderFormat::B8G8R8A8_UNORM; // TODO: Use whatever format the swap chain was created with.
        pipeline_desc.renderTargetCount = 1;
        pipeline_desc.cullMode = RT64::RenderCullMode::NONE;
        pipeline_desc.inputSlots = &vertex_slot_;
        pipeline_desc.inputSlotsCount = 1;
        pipeline_desc.inputElements = vertex_elements.data();
        pipeline_desc.inputElementsCount = uint32_t(vertex_elements.size());
        pipeline_desc.pipelineLayout = layout_.get();
        pipeline_desc.primitiveTopology = RT64::RenderPrimitiveTopology::TRIANGLE_LIST;
        pipeline_desc.vertexShader = vertex_shader_.get();
        pipeline_desc.pixelShader = pixel_shader_.get();

        pipeline_ = render_context->device->createGraphicsPipeline(pipeline_desc);
    }

    void resize_upload_buffer(uint32_t new_size, bool map = true) {
        // Unmap the upload buffer if it's mapped
        if (upload_buffer_mapped_data_ != nullptr) {
            upload_buffer_->unmap();
        }
        
        // If there's already an upload buffer, move it into the stale buffers so it persists until the start of next frame.
        if (upload_buffer_) {
            stale_buffers_.emplace_back(std::move(upload_buffer_));
        }

        // Create the new upload buffer, update the size and map it.
        upload_buffer_ = render_context_->device->createBuffer(RT64::RenderBufferDesc::UploadBuffer(new_size));
        upload_buffer_size_ = new_size;
        upload_buffer_bytes_used_ = 0;
        if (map) {
            upload_buffer_mapped_data_ = reinterpret_cast<uint8_t*>(upload_buffer_->map());
        }
        else {
            upload_buffer_mapped_data_ = nullptr;
        }
    }

    uint32_t allocate_upload_data(uint32_t num_bytes) {
        // Check if there's enough remaining room in the upload buffer to allocate the requested bytes.
        uint32_t total_bytes = num_bytes + upload_buffer_bytes_used_;

        if (total_bytes > upload_buffer_size_) {
            // There isn't, so mark the current upload buffer as stale and allocate a new one with 50% more space than the required amount.
            resize_upload_buffer(total_bytes + total_bytes / 2);
        }

        // Record the current end of the upload buffer to return.
        uint32_t offset = upload_buffer_bytes_used_;

        // Bump the upload buffer's end forward by the number of bytes allocated.
        upload_buffer_bytes_used_ += num_bytes;

        return offset;
    }

    uint32_t allocate_upload_data_aligned(uint32_t num_bytes, uint32_t alignment) {
        // Check if there's enough remaining room in the upload buffer to allocate the requested bytes.
        uint32_t total_bytes = num_bytes + upload_buffer_bytes_used_;

        // Determine the amount of padding needed to meet the target alignment.
        uint32_t padding_bytes = ((upload_buffer_bytes_used_ + alignment - 1) / alignment) * alignment - upload_buffer_bytes_used_;

        // If there isn't enough room to allocate the required bytes plus the padding then resize the upload buffer and allocate from the start of the new one.
        if (total_bytes + padding_bytes > upload_buffer_size_) {
            resize_upload_buffer(total_bytes + total_bytes / 2);

            upload_buffer_bytes_used_ += num_bytes;

            return 0;
        }

        // Otherwise allocate the padding and required bytes and offset the allocated position by the padding size.
        return allocate_upload_data(padding_bytes + num_bytes) + padding_bytes;
    }

    void resize_vertex_buffer(uint32_t new_size) {
        if (vertex_buffer_) {
            stale_buffers_.emplace_back(std::move(vertex_buffer_));
        }
        vertex_buffer_ = render_context_->device->createBuffer(RT64::RenderBufferDesc::VertexBuffer(new_size, RT64::RenderHeapType::DEFAULT));
        vertex_buffer_size_ = new_size;
    }

    void resize_index_buffer(uint32_t new_size) {
        if (index_buffer_) {
            stale_buffers_.emplace_back(std::move(index_buffer_));
        }
        index_buffer_ = render_context_->device->createBuffer(RT64::RenderBufferDesc::IndexBuffer(new_size, RT64::RenderHeapType::DEFAULT));
        index_buffer_size_ = new_size;
    }

    void RenderGeometry(Rml::Vertex* vertices, int num_vertices, int* indices, int num_indices, Rml::TextureHandle texture, const Rml::Vector2f& translation) override {
        uint32_t vert_size_bytes = num_vertices * sizeof(*vertices);
        uint32_t index_size_bytes = num_indices * sizeof(*indices);
        uint32_t total_bytes = vert_size_bytes + index_size_bytes;
        uint32_t index_bytes_start = vert_size_bytes;


        if (!textures_.contains(texture)) {
            if (texture == 0) {
                // Create a 1x1 pixel white texture as the first handle
                Rml::byte white_pixel[] = { 255, 255, 255, 255 };
                create_texture(0, white_pixel, Rml::Vector2i{ 1,1 });
            }
            else {
                assert(false && "Rendered without texture!");
            }
        }

        uint32_t upload_buffer_offset = allocate_upload_data(total_bytes);
        //uint32_t upload_buffer_offset = 0;
        //std::unique_ptr<RT64::RenderBuffer> cur_upload_buffer = render_context_->device->createBuffer(RT64::RenderBufferDesc::UploadBuffer(total_bytes));

        if (vert_size_bytes > vertex_buffer_size_) {
            resize_vertex_buffer(vert_size_bytes + vert_size_bytes / 2);
        }

        if (index_size_bytes > index_buffer_size_) {
            resize_index_buffer(index_size_bytes + index_size_bytes / 2);
        }

        // Copy the vertex and index data into the mapped upload buffer.
        memcpy(upload_buffer_mapped_data_ + upload_buffer_offset, vertices, vert_size_bytes);
        memcpy(upload_buffer_mapped_data_ + upload_buffer_offset + vert_size_bytes, indices, index_size_bytes);

        //uint8_t* buffer_data = reinterpret_cast<uint8_t*>(cur_upload_buffer->map());
        //memcpy(buffer_data, vertices, vert_size_bytes);
        //memcpy(buffer_data + vert_size_bytes, indices, index_size_bytes);
        //cur_upload_buffer->unmap();

        // Prepare the vertex and index buffers for being copied to.
        RT64::RenderBufferBarrier copy_barriers[] = {
			RT64::RenderBufferBarrier::Transition(vertex_buffer_.get(), RT64::RenderBufferState::COPY_DEST),
			RT64::RenderBufferBarrier::Transition(index_buffer_.get(), RT64::RenderBufferState::COPY_DEST)
		};
        list_->barriers(copy_barriers, uint32_t(std::size(copy_barriers)));

        // Copy from the upload buffer to the vertex and index buffers.
        list_->copyBufferRegion(vertex_buffer_->at(0), upload_buffer_->at(upload_buffer_offset), vert_size_bytes);
        list_->copyBufferRegion(index_buffer_->at(0), upload_buffer_->at(upload_buffer_offset + index_bytes_start), index_size_bytes);
         
        //list_->copyBufferRegion(vertex_buffer_->at(0), cur_upload_buffer->at(0), vert_size_bytes);
        //list_->copyBufferRegion(index_buffer_->at(0), cur_upload_buffer->at(0 + index_bytes_start), index_size_bytes);

        //stale_buffers_.emplace_back(std::move(cur_upload_buffer));

        // Prepare the vertex and index buffers for being used for rendering.
        RT64::RenderBufferBarrier usage_barriers[] = {
			RT64::RenderBufferBarrier::Transition(vertex_buffer_.get(), RT64::RenderBufferState::VERTEX_AND_CONSTANT_BUFFER),
			RT64::RenderBufferBarrier::Transition(index_buffer_.get(), RT64::RenderBufferState::INDEX_BUFFER)
		};
        list_->barriers(usage_barriers, uint32_t(std::size(usage_barriers)));

        list_->setViewports(RT64::RenderViewport{ 0, 0, float(window_width_), float(window_height_) });
        if (scissor_enabled_) {
            list_->setScissors(RT64::RenderRect{ scissor_x_, scissor_y_, scissor_width_, scissor_height_ });
        }
        else {
            list_->setScissors(RT64::RenderRect{ 0, 0, window_width_, window_height_ });
        }

        RT64::RenderIndexBufferView index_view{index_buffer_->at(0), index_size_bytes, RT64::RenderFormat::R32_UINT};
        list_->setIndexBuffer(&index_view);
        RT64::RenderVertexBufferView vertex_view{vertex_buffer_->at(0), vert_size_bytes};
        list_->setVertexBuffers(0, &vertex_view, 1, &vertex_slot_);
        list_->setGraphicsDescriptorHeap(textures_.at(texture).heap.get());

        RmlPushConstants constants{
            .transform = mvp_,
            .translation = translation
        };

        list_->setGraphicsPushConstants(0, &constants);

        list_->drawIndexedInstanced(num_indices, 1, 0, 0, 0);
    }

    void EnableScissorRegion(bool enable) override {
        scissor_enabled_ = enable;
    }

    void SetScissorRegion(int x, int y, int width, int height) override {
        scissor_x_ = x;
        scissor_y_ = y;
        scissor_width_ = width;
        scissor_height_ = height;
    }

    bool LoadTexture(Rml::TextureHandle& texture_handle, Rml::Vector2i& texture_dimensions, const Rml::String& source) override {
        std::filesystem::path image_path{ source.c_str() };

        if (image_path.extension() == ".tga") {
            std::vector<char> file_data = read_file(image_path);

            if (file_data.empty()) {
                printf("  File not found or empty\n");
                return false;
            }

            // Make sure ID length is zero
            if (file_data[0] != 0) {
                printf("  Nonzero ID length not supported\n");
                return false;
            }

            // Make sure no color map is used
            if (file_data[1] != 0) {
                printf("  Color maps not supported\n");
                return false;
            }

            // Make sure the image is uncompressed
            if (file_data[2] != 2) {
                printf("  Only uncompressed tga files supported\n");
                return false;
            }

            uint16_t origin_x = from_bytes_le<uint16_t>(file_data.data() + 8);
            uint16_t origin_y = from_bytes_le<uint16_t>(file_data.data() + 10);
            uint16_t size_x = from_bytes_le<uint16_t>(file_data.data() + 12);
            uint16_t size_y = from_bytes_le<uint16_t>(file_data.data() + 14);

            // Nonzero origin not supported
            if (origin_x != 0 || origin_y != 0) {
                printf("  Nonzero origin not supported\n");
                return false;
            }

            uint8_t pixel_depth = file_data[16];

            if (pixel_depth != 32) {
                printf("  Only 32bpp images supported\n");
                return false;
            }

            uint8_t image_descriptor = file_data[17];

            if ((image_descriptor & 0b1111) != 8) {
                printf("  Only 8bpp alpha supported\n");
            }

            if (image_descriptor & 0b110000) {
                printf("  Only bottom-to-top, left-to-right pixel order supported\n");
            }

            texture_dimensions.x = size_x;
            texture_dimensions.y = size_y;

            texture_handle = texture_count_++;
            create_texture(texture_handle, reinterpret_cast<const Rml::byte*>(file_data.data() + 18), texture_dimensions, true);

            return true;
        }

        return false;
    }

    bool GenerateTexture(Rml::TextureHandle& texture_handle, const Rml::byte* source, const Rml::Vector2i& source_dimensions) override {
        texture_handle = texture_count_++;
        return create_texture(texture_handle, source, source_dimensions);
    }

    bool create_texture(Rml::TextureHandle texture_handle, const Rml::byte* source, const Rml::Vector2i& source_dimensions, bool flip_y = false) {
        std::unique_ptr<RT64::RenderTexture> texture =
            render_context_->device->createTexture(RT64::RenderTextureDesc::Texture2D(source_dimensions.x, source_dimensions.y, 1, RmlTextureFormat));

        if (texture != nullptr) {
            uint32_t image_size_bytes = source_dimensions.x * source_dimensions.y * RmlTextureFormatBytesPerPixel;

            // Calculate the texture padding for alignment purposes.
            uint32_t row_pitch = source_dimensions.x * RmlTextureFormatBytesPerPixel;
            uint32_t row_byte_width, row_byte_padding;
            CalculateTextureRowWidthPadding(row_pitch, row_byte_width, row_byte_padding);
            uint32_t row_width = row_byte_width / RmlTextureFormatBytesPerPixel;

            // Calculate the real number of bytes to upload including padding.
            uint32_t uploaded_size_bytes = row_byte_width * source_dimensions.y;

            // Allocate room in the upload buffer for the uploaded data.
            uint32_t upload_buffer_offset = allocate_upload_data_aligned(uploaded_size_bytes, 512);

            // Copy the source data into the upload buffer.
            uint8_t* dst_data = upload_buffer_mapped_data_ + upload_buffer_offset;
                
            if (row_byte_padding == 0) {
                // Copy row-by-row if the image is flipped.
                if (flip_y) {
                    for (uint32_t row = 0; row < source_dimensions.y; row++) {
                        memcpy(dst_data + row_byte_width * (source_dimensions.y - row - 1), source + row_byte_width * row, row_byte_width);
                    }
                }
                // Directly copy if no padding is needed and the image isn't flipped.
                else {
                    memcpy(dst_data, source, image_size_bytes);
                }
            }
            // Otherwise pad each row as necessary.
            else {
                const Rml::byte *src_data = flip_y ? source + row_pitch * (source_dimensions.y - 1) : source;
                uint32_t src_stride = flip_y ? -row_pitch : row_pitch;
                size_t offset = 0;

                for (uint32_t row = 0; row < source_dimensions.y; row++) { //(offset + increment) <= image_size_bytes) {
                    memcpy(dst_data, src_data, row_pitch);
                    src_data += src_stride;
                    offset += row_pitch;
                    dst_data += row_byte_width;
                }
            }

            // Prepare the texture to be a destination for copying.
            list_->barriers(
                RT64::RenderTextureBarrier::Transition(texture.get(), RT64::RenderTextureState::COPY_DEST));
            
            // Copy the upload buffer into the texture.
            list_->copyTextureRegion(
                RT64::RenderTextureCopyLocation::Subresource(texture.get()),
                RT64::RenderTextureCopyLocation::PlacedFootprint(upload_buffer_.get(), RmlTextureFormat, source_dimensions.x, source_dimensions.y, 1, row_width, upload_buffer_offset));
            
            // Prepare the texture for being read from a pixel shader.
            list_->barriers(RT64::RenderTextureBarrier::Transition(texture.get(), RT64::RenderTextureState::PIXEL_SHADER_ACCESS));

            // Create a descriptor heap with this texture in it.
            std::unique_ptr<RT64::RenderDescriptorHeap> heap = heap_base_->builder.create(render_context_->device);

            heap->setTexture(heap_base_->gTexture, 0, texture.get(), RT64::RenderTextureState::PIXEL_SHADER_ACCESS);

            textures_.emplace(texture_handle, TextureHandle{ std::move(texture), std::move(heap) });

            return true;
        }

        return false;
    }

	void ReleaseTexture(Rml::TextureHandle texture) override {
        textures_.erase(texture);
    }

    void SetTransform(const Rml::Matrix4f* transform) override {
        transform_ = transform ? *transform : Rml::Matrix4f::Identity();
        recalculate_mvp();
    }

    void recalculate_mvp() {
        mvp_ = projection_mtx_ * transform_;
    }

    void start(RT64::RenderCommandList* list, uint32_t image_width, uint32_t image_height) {
        list_ = list;
        list_->setPipeline(pipeline_.get());
        list_->setGraphicsPipelineLayout(layout_.get());

        window_width_ = image_width;
        window_height_ = image_height;

        projection_mtx_ = Rml::Matrix4f::ProjectOrtho(0.0f, static_cast<float>(image_width), static_cast<float>(image_height), 0.0f, -10000, 10000);
        recalculate_mvp();

        // The following code assumes command lists aren't double buffered.
        // Clear out any stale buffers from the last command list.
        stale_buffers_.clear();

        // Reset and map the upload buffer.
        upload_buffer_bytes_used_ = 0;
        upload_buffer_mapped_data_ = reinterpret_cast<uint8_t*>(upload_buffer_->map());
    }

    void end(RT64::RenderCommandList* list) {
        list_ = nullptr;

        // Unmap the upload buffer if it's mapped.
        if (upload_buffer_mapped_data_) {
            upload_buffer_->unmap();
            upload_buffer_mapped_data_ = nullptr;
        }
    }
};

struct {
    struct UIRenderContext render;
    class {
        std::unordered_map<Menu, Rml::ElementDocument*> documents;
        Rml::ElementDocument* current_document;
    public:
        SystemInterface_SDL system_interface;
        std::unique_ptr<RmlRenderInterface_RT64> render_interface;
        Rml::Context* context;
        std::unique_ptr<Rml::EventListenerInstancer> event_listener_instancer;

        void swap_document(Menu menu) {
            if (current_document != nullptr) {
                current_document->Hide();
            }

            auto find_it = documents.find(menu);
            if (find_it != documents.end()) {
                assert(find_it->second && "Document for menu not loaded!");
                current_document = find_it->second;
                current_document->Show();
            }
            else {
                current_document = nullptr;
            }
        }

        void load_documents() {
            if (!documents.empty()) {
                Rml::Factory::RegisterEventListenerInstancer(nullptr);
                for (auto doc : documents) {
                    doc.second->ReloadStyleSheet();
                }

                Rml::ReleaseTextures();
                Rml::ReleaseMemoryPools();

                if (current_document != nullptr) {
                    current_document->Hide();
                    current_document->Close();
                }

                current_document = nullptr;

                documents.clear();
                Rml::Factory::RegisterEventListenerInstancer(event_listener_instancer.get());
            }

            documents.emplace(Menu::Launcher, context->LoadDocument("assets/launcher.rml"));
        }
    } rml;
} UIContext;

// TODO make this not be global
extern SDL_Window* window;

void init_hook(RT64::RenderInterface* interface, RT64::RenderDevice* device) {
    printf("RT64 hook init\n");

    UIContext.render.interface = interface;
    UIContext.render.device = device;

    // Setup RML
    UIContext.rml.system_interface.SetWindow(window);
    UIContext.rml.render_interface = std::make_unique<RmlRenderInterface_RT64>(&UIContext.render);
    UIContext.rml.event_listener_instancer = make_event_listener_instancer();

    Rml::SetSystemInterface(&UIContext.rml.system_interface);
    Rml::SetRenderInterface(UIContext.rml.render_interface.get());
    Rml::Factory::RegisterEventListenerInstancer(UIContext.rml.event_listener_instancer.get());

    Rml::Initialise();

    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    
    UIContext.rml.context = Rml::CreateContext("main", Rml::Vector2i(width, height));

    Rml::Debugger::Initialise(UIContext.rml.context);

    {
        const Rml::String directory = "assets/";

        struct FontFace {
            const char* filename;
            bool fallback_face;
        };
        FontFace font_faces[] = {
            {"LatoLatin-Regular.ttf", false},
            {"LatoLatin-Italic.ttf", false},
            {"LatoLatin-Bold.ttf", false},
            {"LatoLatin-BoldItalic.ttf", false},
            {"NotoEmoji-Regular.ttf", true},
        };

        for (const FontFace& face : font_faces) {
            Rml::LoadFontFace(directory + face.filename, face.fallback_face);
        }
    }

    UIContext.rml.load_documents();
}

moodycamel::ConcurrentQueue<SDL_Event> ui_event_queue{};

void queue_event(const SDL_Event& event) {
    ui_event_queue.enqueue(event);
}

bool try_deque_event(SDL_Event& out) {
    return ui_event_queue.try_dequeue(out);
}

std::atomic<Menu> open_menu = Menu::Launcher;

void draw_hook(RT64::RenderCommandList* command_list, RT64::RenderTexture* swap_chain_texture) {
    int num_keys;
    const Uint8* key_state = SDL_GetKeyboardState(&num_keys);

    static bool was_reload_held = false;
    bool is_reload_held = key_state[SDL_SCANCODE_F11] != 0;
    bool reload_sheets = is_reload_held && !was_reload_held;
    was_reload_held = is_reload_held;
    
    static Menu prev_menu = Menu::None;
    Menu cur_menu = open_menu.load();

    if (reload_sheets) {
        UIContext.rml.load_documents();
        prev_menu = Menu::None;
    }

    if (cur_menu != prev_menu) {
        UIContext.rml.swap_document(cur_menu);
    }

    prev_menu = cur_menu;

    SDL_Event cur_event{};

    while (try_deque_event(cur_event)) {
        RmlSDL::InputEventHandler(UIContext.rml.context, cur_event);
    }

    if (cur_menu != Menu::None) {
        int width, height;
        SDL_GetWindowSizeInPixels(window, &width, &height);

        UIContext.rml.render_interface->start(command_list, width, height);

        static int prev_width = 0;
        static int prev_height = 0;

        if (prev_width != width || prev_height != height) {
            UIContext.rml.context->SetDimensions({ width, height });
        }
        prev_width = width;
        prev_height = height;

        UIContext.rml.context->Update();
        UIContext.rml.context->Render();
        UIContext.rml.render_interface->end(command_list);
    }
}

void deinit_hook() {

}

void set_rt64_hooks() {
    RT64::SetRenderHooks(init_hook, draw_hook, deinit_hook);
}

void set_current_menu(Menu menu) {
    open_menu.store(menu);
}
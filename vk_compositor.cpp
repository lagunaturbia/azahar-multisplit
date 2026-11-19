// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_compositor.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
constexpr auto color_range = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr auto color_layers = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
constexpr auto compose_format = VK_FORMAT_R8G8B8A8_UNORM;

template<typename T>
T info(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (Vulkan " +
                                 std::to_string(result) + ").");
}

struct Source {
    VkImage image{};
    VkImageLayout layout{};
    uint32_t width{};
    uint32_t height{};
    bool unsupported{};
};

struct Frame {
    VkDeviceMemory memory{};
    retro_vulkan_image output{};
    // The frontend may keep the above view descriptor for duplicated frames.
    VkCommandBuffer commands{};
    VkFence submitted{};
    bool used{};
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};
    void* mapped{};
    unsigned menu_selection{};
};
}

struct VulkanCompositor::State {
    retro_hw_render_interface_vulkan api;
    std::array<Source, 4> sources{};
    std::array<Frame, 32> frames{};
    VkCommandPool pool{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties{};
    uint32_t side;
    uint32_t height;
    unsigned players;
    uint32_t width;
    VkFormat format;

#define VK_COMPOSITOR_FUNCTIONS(X)                                                              \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer)    \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage)                \
    X(CmdBlitImage) X(CmdClearColorImage) X(CreateImage) X(DestroyImage) X(CreateImageView)         \
    X(DestroyImageView) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory)             \
    X(BindImageMemory) X(QueueSubmit) X(CreateFence) X(DestroyFence) X(WaitForFences)              \
    X(ResetFences) X(DeviceWaitIdle) X(CreateBuffer) X(DestroyBuffer)                             \
    X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory)               \
    X(CmdCopyBufferToImage)
#define DECLARE(name) PFN_vk##name vk##name{};
    VK_COMPOSITOR_FUNCTIONS(DECLARE)
#undef DECLARE

    State(const retro_hw_render_interface_vulkan& interface, unsigned tile, unsigned rows,
          unsigned count)
        : api(interface), side(tile), height(rows), players(count), width(tile * std::max(1u, count)),
          format(count ? compose_format : VK_FORMAT_B8G8R8A8_UNORM) {
        if (!api.device || !api.gpu || !api.queue || !api.get_device_proc_addr ||
            !api.get_instance_proc_addr || !api.get_sync_index || !api.wait_sync_index ||
            !api.set_image || !api.lock_queue || !api.unlock_queue)
            throw std::runtime_error("RetroArch returned an incomplete Vulkan interface.");
#define LOAD(name)                                                                              \
        vk##name = reinterpret_cast<PFN_vk##name>(api.get_device_proc_addr(api.device, "vk" #name));\
        if (!vk##name)                                                                          \
            throw std::runtime_error("Missing Vulkan command vk" #name ".");
        VK_COMPOSITOR_FUNCTIONS(LOAD)
#undef LOAD
#undef VK_COMPOSITOR_FUNCTIONS
        get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            api.get_instance_proc_addr(api.instance, "vkGetPhysicalDeviceMemoryProperties"));
        if (!get_memory_properties)
            throw std::runtime_error("Missing Vulkan physical device memory properties.");
        get_memory_properties(api.gpu, &memory_properties);
        auto descriptor = info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        descriptor.queueFamilyIndex = api.queue_index;
        descriptor.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(api.device, &descriptor, nullptr, &pool), "vkCreateCommandPool");
    }

    ~State() {
        // The frontend may still be sampling the last image.
        api.lock_queue(api.handle);
        vkDeviceWaitIdle(api.device);
        api.unlock_queue(api.handle);
        for (auto& frame : frames) {
            if (frame.mapped)
                vkUnmapMemory(api.device, frame.staging_memory);
            if (frame.staging)
                vkDestroyBuffer(api.device, frame.staging, nullptr);
            if (frame.staging_memory)
                vkFreeMemory(api.device, frame.staging_memory, nullptr);
            if (frame.submitted)
                vkDestroyFence(api.device, frame.submitted, nullptr);
            if (frame.output.image_view)
                vkDestroyImageView(api.device, frame.output.image_view, nullptr);
            if (frame.output.create_info.image)
                vkDestroyImage(api.device, frame.output.create_info.image, nullptr);
            if (frame.memory)
                vkFreeMemory(api.device, frame.memory, nullptr);
        }
        vkDestroyCommandPool(api.device, pool, nullptr);
    }

    uint32_t find_memory(uint32_t mask, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((mask & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        throw std::runtime_error("There is no compatible memory for the Vulkan compositor.");
    }

    void create_frame(Frame& frame) {
        auto descriptor = info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        descriptor.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        descriptor.imageType = VK_IMAGE_TYPE_2D;
        descriptor.format = format;
        descriptor.extent = {width, height, 1};
        descriptor.mipLevels = 1;
        descriptor.arrayLayers = 1;
        descriptor.samples = VK_SAMPLE_COUNT_1_BIT;
        descriptor.tiling = VK_IMAGE_TILING_OPTIMAL;
        descriptor.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        descriptor.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        auto& view = frame.output.create_info;
        view = info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        check(vkCreateImage(api.device, &descriptor, nullptr, &view.image), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(api.device, view.image, &requirements);
        auto memory = info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        memory.allocationSize = requirements.size;
        memory.memoryTypeIndex = find_memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(api.device, &memory, nullptr, &frame.memory), "vkAllocateMemory");
        check(vkBindImageMemory(api.device, view.image, frame.memory, 0), "vkBindImageMemory");

        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = color_range;
        check(vkCreateImageView(api.device, &view, nullptr, &frame.output.image_view),
              "vkCreateImageView");
        frame.output.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        auto allocate = info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(api.device, &allocate, &frame.commands),
              "vkAllocateCommandBuffers");
        const auto fence = info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        check(vkCreateFence(api.device, &fence, nullptr, &frame.submitted), "vkCreateFence");
    }

    void transition(VkCommandBuffer commands, VkImage image, VkImageLayout before, VkImageLayout after,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                    VkAccessFlags src_access, VkAccessFlags dst_access) {
        auto barrier = info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = color_range;
        vkCmdPipelineBarrier(commands, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void copy_source(Frame& frame, const Source& source, unsigned player) {
        const auto commands = frame.commands;
        const auto output = frame.output.create_info.image;
        transition(commands, source.image, source.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const auto left = static_cast<int32_t>(side * player);
        if (source.width == side && source.height == height) {
            const VkImageCopy region{color_layers, {0, 0, 0}, color_layers, {left, 0, 0},
                                     {side, height, 1}};
            vkCmdCopyImage(commands, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        } else {
            const VkImageBlit region{color_layers,
                {{0, 0, 0}, {static_cast<int32_t>(source.width), static_cast<int32_t>(source.height), 1}},
                color_layers, {{left, 0, 0}, {left + static_cast<int32_t>(side),
                                               static_cast<int32_t>(height), 1}}};
            vkCmdBlitImage(commands, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        }
        transition(commands, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.layout,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }

    void upload_menu(Frame& frame, const uint32_t* pixels, unsigned selection) {
        const auto size = static_cast<VkDeviceSize>(width) * height * sizeof(uint32_t);
        if (!frame.staging) {
            auto buffer = info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
            buffer.size = size;
            buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateBuffer(api.device, &buffer, nullptr, &frame.staging), "vkCreateBuffer");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(api.device, frame.staging, &requirements);
            auto memory = info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
            memory.allocationSize = requirements.size;
            memory.memoryTypeIndex = find_memory(requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            check(vkAllocateMemory(api.device, &memory, nullptr, &frame.staging_memory),
                  "vkAllocateMemory");
            check(vkBindBufferMemory(api.device, frame.staging, frame.staging_memory, 0),
                  "vkBindBufferMemory");
            check(vkMapMemory(api.device, frame.staging_memory, 0, size, 0, &frame.mapped),
                  "vkMapMemory");
        }
        if (frame.menu_selection != selection) {
            std::memcpy(frame.mapped, pixels, static_cast<size_t>(size));
            frame.menu_selection = selection;
        }
        const VkBufferImageCopy region{0, 0, 0, color_layers, {0, 0, 0}, {width, height, 1}};
        vkCmdCopyBufferToImage(frame.commands, frame.staging, frame.output.create_info.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    bool compose(const uint32_t* menu_pixels = nullptr, unsigned selection = 0) {
        for (const auto& source : sources) {
            if (source.unsupported)
                throw std::runtime_error("Unexpected Azahar Vulkan format, semaphore or queue family.");
        }

        const uint32_t index = api.get_sync_index(api.handle);
        if (index >= frames.size())
            throw std::runtime_error("RetroArch returned an invalid Vulkan sync index.");
        auto& frame = frames[index];
        const bool reuse = frame.used;
        if (!frame.commands)
            create_frame(frame);
        if (reuse) {
            api.wait_sync_index(api.handle);
            check(vkWaitForFences(api.device, 1, &frame.submitted, VK_TRUE, UINT64_MAX),
                  "vkWaitForFences");
            check(vkResetFences(api.device, 1, &frame.submitted), "vkResetFences");
            check(vkResetCommandBuffer(frame.commands, 0), "vkResetCommandBuffer");
        }

        auto begin = info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(frame.commands, &begin), "vkBeginCommandBuffer");
        const auto output = frame.output.create_info.image;
        transition(frame.commands, output, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT);

        if (menu_pixels) {
            upload_menu(frame, menu_pixels, selection);
        } else if (std::any_of(sources.begin(), sources.begin() + players, [](const auto& source) {
            return !source.image || !source.width;
        })) {
            const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 1.0f}};
            vkCmdClearColorImage(frame.commands, output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &clear, 1, &color_range);
            transition(frame.commands, output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        for (unsigned player = 0; player < players; ++player) {
            const auto& source = sources[player];
            if (source.image && source.width && source.height)
                copy_source(frame, source, player);
        }

        transition(frame.commands, output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   frame.output.image_layout, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        check(vkEndCommandBuffer(frame.commands), "vkEndCommandBuffer");

        auto submit = info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.commands;
        api.lock_queue(api.handle);
        const VkResult result = vkQueueSubmit(api.queue, 1, &submit, frame.submitted);
        api.unlock_queue(api.handle);
        check(result, "vkQueueSubmit");
        frame.used = true;

        api.set_image(api.handle, &frame.output, 0, nullptr, api.queue_index);
        return true;
    }
};

VulkanCompositor::VulkanCompositor() = default;
VulkanCompositor::~VulkanCompositor() = default;

void VulkanCompositor::initialize(const retro_hw_render_interface_vulkan& frontend, unsigned side,
                                  unsigned height, unsigned players) {
    reset();
    state = std::make_unique<State>(frontend, side, height, players);
}

void VulkanCompositor::reset() { state.reset(); }
bool VulkanCompositor::ready() const { return static_cast<bool>(state); }
void VulkanCompositor::clear_sources() {
    if (state)
        state->sources = {};
}

void VulkanCompositor::capture(unsigned player, const retro_vulkan_image* image,
                               uint32_t semaphore_count, uint32_t queue_family) {
    if (!state || !image)
        return;
    auto& source = state->sources[player];
    source.image = image->create_info.image;
    source.layout = image->image_layout;
    source.unsupported = semaphore_count != 0 || image->create_info.format != compose_format ||
                         (queue_family != VK_QUEUE_FAMILY_IGNORED && queue_family != state->api.queue_index);
}
void VulkanCompositor::set_size(unsigned player, unsigned width, unsigned height) {
    if (state) {
        state->sources[player].width = width;
        state->sources[player].height = height;
    }
}
bool VulkanCompositor::compose() { return state && state->compose(); }
bool VulkanCompositor::draw_menu(const uint32_t* pixels, unsigned selection) {
    return state && state->compose(pixels, selection);
}

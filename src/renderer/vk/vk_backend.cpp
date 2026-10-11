#define VK_NO_PROTOTYPES
#include "vk_backend.hpp"
#include "../bc_decode.hpp"
#include "../msl_to_glsl.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#if defined(__ANDROID__)
#include <glslang/Include/glslang_c_interface.h>
#else
#include <shaderc/shaderc.hpp>
#endif

#if !defined(_WIN32)
#include <dlfcn.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#define VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME "VK_KHR_portability_subset"
#endif

namespace me {
namespace vk_gl {

namespace {

// -----------------------------------------------------------------------------
// Dynamic Vulkan Dispatch Table (Zero Link-Time libvulkan Dependency)
// -----------------------------------------------------------------------------
struct VkDispatch {
    void* lib = nullptr;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;

    // Global
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;

    // Instance
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;

    // Device
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreatePipelineCache CreatePipelineCache = nullptr;
    PFN_vkDestroyPipelineCache DestroyPipelineCache = nullptr;
    PFN_vkGetPipelineCacheData GetPipelineCacheData = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkResetDescriptorPool ResetDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkResetCommandPool ResetCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
    PFN_vkCmdSetViewport CmdSetViewport = nullptr;
    PFN_vkCmdSetScissor CmdSetScissor = nullptr;
    PFN_vkCmdSetDepthBias CmdSetDepthBias = nullptr;
    PFN_vkCmdClearAttachments CmdClearAttachments = nullptr;
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
    PFN_vkCmdCopyBuffer CmdCopyBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdBlitImage CmdBlitImage = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;

    bool load_library(bool windowed) {
        if (GetInstanceProcAddr) return true;
        if (windowed) {
            void* proc = SDL_Vulkan_GetVkGetInstanceProcAddr();
            if (proc) {
                GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc);
            }
        }
        if (!GetInstanceProcAddr) {
#if defined(_WIN32)
            HMODULE mod = LoadLibraryA("vulkan-1.dll");
            if (mod) {
                lib = reinterpret_cast<void*>(mod);
                GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mod, "vkGetInstanceProcAddr"));
            }
#else
            static const char* const kCandidates[] = {
#if defined(__APPLE__)
                "/opt/homebrew/lib/libMoltenVK.dylib",
                "/opt/homebrew/lib/libvulkan.1.dylib",
                "libMoltenVK.dylib",
                "libvulkan.1.dylib",
#elif defined(__ANDROID__)
                "libvulkan.so",
#else
                "libvulkan.so.1",
                "libvulkan.so",
#endif
            };
            for (const char* name : kCandidates) {
                void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
                if (h) {
                    void* sym = dlsym(h, "vkGetInstanceProcAddr");
                    if (sym) {
                        lib = h;
                        GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(sym);
                        break;
                    }
                    dlclose(h);
                }
            }
#endif
        }
        if (!GetInstanceProcAddr) return false;
        CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        EnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            GetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
        return CreateInstance != nullptr && EnumerateInstanceExtensionProperties != nullptr;
    }

    bool load_instance(VkInstance inst) {
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetInstanceProcAddr(inst, name));
        };
        get(DestroyInstance, "vkDestroyInstance");
        get(EnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
        get(GetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
        get(GetPhysicalDeviceFeatures, "vkGetPhysicalDeviceFeatures");
        get(GetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
        get(GetPhysicalDeviceFormatProperties, "vkGetPhysicalDeviceFormatProperties");
        get(GetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
        get(EnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
        get(CreateDevice, "vkCreateDevice");
        get(GetDeviceProcAddr, "vkGetDeviceProcAddr");
        get(DestroySurfaceKHR, "vkDestroySurfaceKHR");
        get(GetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR");
        get(GetPhysicalDeviceSurfaceCapabilitiesKHR, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        get(GetPhysicalDeviceSurfaceFormatsKHR, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        get(GetPhysicalDeviceSurfacePresentModesKHR, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        return EnumeratePhysicalDevices && CreateDevice && GetDeviceProcAddr;
    }

    bool load_device(VkDevice dev) {
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetDeviceProcAddr(dev, name));
        };
        get(DestroyDevice, "vkDestroyDevice");
        get(GetDeviceQueue, "vkGetDeviceQueue");
        get(DeviceWaitIdle, "vkDeviceWaitIdle");
        get(QueueWaitIdle, "vkQueueWaitIdle");
        get(QueueSubmit, "vkQueueSubmit");
        get(AllocateMemory, "vkAllocateMemory");
        get(FreeMemory, "vkFreeMemory");
        get(MapMemory, "vkMapMemory");
        get(UnmapMemory, "vkUnmapMemory");
        get(CreateBuffer, "vkCreateBuffer");
        get(DestroyBuffer, "vkDestroyBuffer");
        get(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
        get(BindBufferMemory, "vkBindBufferMemory");
        get(CreateImage, "vkCreateImage");
        get(DestroyImage, "vkDestroyImage");
        get(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
        get(BindImageMemory, "vkBindImageMemory");
        get(CreateImageView, "vkCreateImageView");
        get(DestroyImageView, "vkDestroyImageView");
        get(CreateSampler, "vkCreateSampler");
        get(DestroySampler, "vkDestroySampler");
        get(CreateShaderModule, "vkCreateShaderModule");
        get(DestroyShaderModule, "vkDestroyShaderModule");
        get(CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
        get(DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
        get(CreatePipelineLayout, "vkCreatePipelineLayout");
        get(DestroyPipelineLayout, "vkDestroyPipelineLayout");
        get(CreatePipelineCache, "vkCreatePipelineCache");
        get(DestroyPipelineCache, "vkDestroyPipelineCache");
        get(GetPipelineCacheData, "vkGetPipelineCacheData");
        get(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
        get(DestroyPipeline, "vkDestroyPipeline");
        get(CreateRenderPass, "vkCreateRenderPass");
        get(DestroyRenderPass, "vkDestroyRenderPass");
        get(CreateFramebuffer, "vkCreateFramebuffer");
        get(DestroyFramebuffer, "vkDestroyFramebuffer");
        get(CreateDescriptorPool, "vkCreateDescriptorPool");
        get(DestroyDescriptorPool, "vkDestroyDescriptorPool");
        get(ResetDescriptorPool, "vkResetDescriptorPool");
        get(AllocateDescriptorSets, "vkAllocateDescriptorSets");
        get(UpdateDescriptorSets, "vkUpdateDescriptorSets");
        get(CreateCommandPool, "vkCreateCommandPool");
        get(DestroyCommandPool, "vkDestroyCommandPool");
        get(ResetCommandPool, "vkResetCommandPool");
        get(AllocateCommandBuffers, "vkAllocateCommandBuffers");
        get(FreeCommandBuffers, "vkFreeCommandBuffers");
        get(BeginCommandBuffer, "vkBeginCommandBuffer");
        get(EndCommandBuffer, "vkEndCommandBuffer");
        get(ResetCommandBuffer, "vkResetCommandBuffer");
        get(CmdPipelineBarrier, "vkCmdPipelineBarrier");
        get(CmdBeginRenderPass, "vkCmdBeginRenderPass");
        get(CmdEndRenderPass, "vkCmdEndRenderPass");
        get(CmdBindPipeline, "vkCmdBindPipeline");
        get(CmdBindDescriptorSets, "vkCmdBindDescriptorSets");
        get(CmdBindVertexBuffers, "vkCmdBindVertexBuffers");
        get(CmdBindIndexBuffer, "vkCmdBindIndexBuffer");
        get(CmdSetViewport, "vkCmdSetViewport");
        get(CmdSetScissor, "vkCmdSetScissor");
        get(CmdSetDepthBias, "vkCmdSetDepthBias");
        get(CmdClearAttachments, "vkCmdClearAttachments");
        get(CmdDraw, "vkCmdDraw");
        get(CmdDrawIndexed, "vkCmdDrawIndexed");
        get(CmdCopyBuffer, "vkCmdCopyBuffer");
        get(CmdCopyBufferToImage, "vkCmdCopyBufferToImage");
        get(CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
        get(CmdCopyImage, "vkCmdCopyImage");
        get(CmdBlitImage, "vkCmdBlitImage");
        get(CreateFence, "vkCreateFence");
        get(DestroyFence, "vkDestroyFence");
        get(WaitForFences, "vkWaitForFences");
        get(ResetFences, "vkResetFences");
        get(CreateSemaphore, "vkCreateSemaphore");
        get(DestroySemaphore, "vkDestroySemaphore");
        get(CreateSwapchainKHR, "vkCreateSwapchainKHR");
        get(DestroySwapchainKHR, "vkDestroySwapchainKHR");
        get(GetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
        get(AcquireNextImageKHR, "vkAcquireNextImageKHR");
        get(QueuePresentKHR, "vkQueuePresentKHR");
        return DestroyDevice && GetDeviceQueue && CreateGraphicsPipelines && CmdDraw;
    }
};

// -----------------------------------------------------------------------------
// Thread-Safe SPIR-V Compiler & Persistent Disk / Memory Cache
// -----------------------------------------------------------------------------
uint64_t fnv1a_hash(const std::string& s, bool is_vertex) {
    uint64_t h = is_vertex ? 0xcbf29ce484222325ULL : 0x84222325cbf29ce4ULL;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

class SpirvCache {
public:
    std::mutex mu;
    std::unordered_map<uint64_t, std::vector<uint32_t>> map;
    std::string cache_path;
    bool dirty = false;

    void load_from_disk(const std::string& dir) {
        std::lock_guard<std::mutex> lock(mu);
        if (dir.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        cache_path = (std::filesystem::path(dir) / "vk_spirv_cache_v1.bin").string();
        std::ifstream f(cache_path, std::ios::binary);
        if (!f.is_open()) return;
        uint32_t magic = 0, count = 0;
        f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        f.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (magic != 0x5650534Du || count > 65536) return;
        for (uint32_t i = 0; i < count && f.good(); ++i) {
            uint64_t hash = 0;
            uint32_t words = 0;
            f.read(reinterpret_cast<char*>(&hash), sizeof(hash));
            f.read(reinterpret_cast<char*>(&words), sizeof(words));
            if (!f.good() || words == 0 || words > 1024 * 1024) break;
            std::vector<uint32_t> spv(words);
            f.read(reinterpret_cast<char*>(spv.data()), static_cast<std::streamsize>(words * sizeof(uint32_t)));
            if (!f.good()) break;
            map[hash] = std::move(spv);
        }
    }

    void save_to_disk() {
        std::lock_guard<std::mutex> lock(mu);
        if (!dirty || cache_path.empty()) return;
        std::ofstream f(cache_path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return;
        const uint32_t magic = 0x5650534Du;
        const uint32_t count = static_cast<uint32_t>(map.size());
        f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        f.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& [hash, spv] : map) {
            const uint32_t words = static_cast<uint32_t>(spv.size());
            f.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
            f.write(reinterpret_cast<const char*>(&words), sizeof(words));
            f.write(reinterpret_cast<const char*>(spv.data()), static_cast<std::streamsize>(words * sizeof(uint32_t)));
        }
        dirty = false;
    }

    bool lookup(uint64_t hash, std::vector<uint32_t>& out) {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = map.find(hash);
        if (it == map.end()) return false;
        out = it->second;
        return true;
    }

    void insert(uint64_t hash, const std::vector<uint32_t>& spv) {
        std::lock_guard<std::mutex> lock(mu);
        if (!map.count(hash)) {
            map[hash] = spv;
            dirty = true;
        }
    }
};

bool compile_glsl_to_spirv_uncached(const std::string& glsl, bool is_vertex, std::vector<uint32_t>& out_spirv,
                                    std::string& out_log) {
#if defined(__ANDROID__)
    static std::once_flag init_once;
    std::call_once(init_once, []() { glslang_initialize_process(); });

    static const glslang_resource_t kAndroidResource = {
        .max_lights = 8,
        .max_clip_planes = 6,
        .max_texture_units = 32,
        .max_texture_coords = 32,
        .max_vertex_attribs = 16,
        .max_vertex_uniform_components = 4096,
        .max_varying_floats = 64,
        .max_vertex_texture_image_units = 32,
        .max_combined_texture_image_units = 80,
        .max_texture_image_units = 32,
        .max_fragment_uniform_components = 4096,
        .max_draw_buffers = 8,
        .max_vertex_uniform_vectors = 256,
        .max_varying_vectors = 16,
        .max_fragment_uniform_vectors = 256,
        .max_vertex_output_vectors = 16,
        .max_fragment_input_vectors = 16,
        .min_program_texel_offset = -8,
        .max_program_texel_offset = 7,
        .max_clip_distances = 8,
        .max_compute_work_group_count_x = 65535,
        .max_compute_work_group_count_y = 65535,
        .max_compute_work_group_count_z = 65535,
        .max_compute_work_group_size_x = 1024,
        .max_compute_work_group_size_y = 1024,
        .max_compute_work_group_size_z = 64,
        .max_compute_uniform_components = 1024,
        .max_compute_texture_image_units = 16,
        .max_compute_image_uniforms = 8,
        .max_compute_atomic_counters = 8,
        .max_compute_atomic_counter_buffers = 1,
        .max_varying_components = 64,
        .max_vertex_output_components = 64,
        .max_geometry_input_components = 64,
        .max_geometry_output_components = 128,
        .max_fragment_input_components = 128,
        .max_image_units = 8,
        .max_combined_image_units_and_fragment_outputs = 8,
        .max_combined_shader_output_resources = 8,
        .max_fragment_image_uniforms = 8,
        .max_combined_image_uniforms = 8,
        .max_geometry_texture_image_units = 16,
        .max_geometry_output_vertices = 256,
        .max_geometry_total_output_components = 1024,
        .max_geometry_uniform_components = 1024,
        .max_geometry_varying_components = 64,
        .max_tess_control_input_components = 128,
        .max_tess_control_output_components = 128,
        .max_tess_control_texture_image_units = 16,
        .max_tess_control_uniform_components = 1024,
        .max_tess_control_total_output_components = 4096,
        .max_tess_evaluation_input_components = 128,
        .max_tess_evaluation_output_components = 128,
        .max_tess_evaluation_texture_image_units = 16,
        .max_tess_evaluation_uniform_components = 1024,
        .max_tess_patch_components = 120,
        .max_patch_vertices = 32,
        .max_tess_gen_level = 64,
        .max_viewports = 16,
        .max_fragment_atomic_counters = 8,
        .max_combined_atomic_counters = 8,
        .max_atomic_counter_bindings = 1,
        .max_combined_atomic_counter_buffers = 1,
        .max_atomic_counter_buffer_size = 16384,
        .max_transform_feedback_buffers = 4,
        .max_transform_feedback_interleaved_components = 64,
        .max_cull_distances = 8,
        .max_combined_clip_and_cull_distances = 8,
        .max_samples = 4,
        .maxDualSourceDrawBuffersEXT = 1,
        .limits = {true, true, true, true, true, true, true, true, true},
    };

    const glslang_stage_t stage = is_vertex ? GLSLANG_STAGE_VERTEX : GLSLANG_STAGE_FRAGMENT;
    const glslang_input_t input = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = stage,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = GLSLANG_TARGET_VULKAN_1_0,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = GLSLANG_TARGET_SPV_1_0,
        .code = glsl.c_str(),
        .default_version = 450,
        .default_profile = GLSLANG_NO_PROFILE,
        .force_default_version_and_profile = false,
        .forward_compatible = false,
        .messages = GLSLANG_MSG_DEFAULT_BIT,
        .resource = &kAndroidResource,
    };
    glslang_shader_t* shader = glslang_shader_create(&input);
    if (!shader) {
        out_log = "glslang_shader_create failed";
        return false;
    }
    if (!glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input)) {
        const char* info = glslang_shader_get_info_log(shader);
        out_log = info ? info : "glslang preprocess/parse failed";
        glslang_shader_delete(shader);
        return false;
    }
    glslang_program_t* prog = glslang_program_create();
    glslang_program_add_shader(prog, shader);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        const char* info = glslang_program_get_info_log(prog);
        out_log = info ? info : "glslang link failed";
        glslang_program_delete(prog);
        glslang_shader_delete(shader);
        return false;
    }
    glslang_program_SPIRV_generate(prog, stage);
    const size_t words = glslang_program_SPIRV_get_size(prog);
    out_spirv.resize(words);
    glslang_program_SPIRV_get(prog, out_spirv.data());
    glslang_program_delete(prog);
    glslang_shader_delete(shader);
    return !out_spirv.empty();
#else
    shaderc::Compiler compiler;
    shaderc::CompileOptions opts;
    opts.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    opts.SetOptimizationLevel(shaderc_optimization_level_performance);
    const shaderc_shader_kind kind = is_vertex ? shaderc_glsl_vertex_shader : shaderc_glsl_fragment_shader;
    const shaderc::SpvCompilationResult res = compiler.CompileGlslToSpv(glsl, kind, "me_shader.glsl", opts);
    if (res.GetCompilationStatus() != shaderc_compilation_status_success) {
        out_log = res.GetErrorMessage();
        return false;
    }
    out_spirv.assign(res.cbegin(), res.cend());
    return !out_spirv.empty();
#endif
}

// -----------------------------------------------------------------------------
// Reflected GLSL Bindings (Parsed Directly from Emitted GLSL 450 Source)
// -----------------------------------------------------------------------------
struct ReflectedUbo {
    uint32_t binding = 0;  // SPIR-V binding in set 0
    std::string block_name;
    GLuint gl_slot = 0;    // bound slot passed to glUniformBlockBinding / glBindBufferRange
};

struct ReflectedSampler {
    uint32_t binding = 0;  // SPIR-V binding in set 0
    std::string name;
    bool is_cube = false;
    bool is_array = false;
    bool is_shadow = false;
    GLint bound_unit = 0;  // texture unit set via glUniform1i
};

void reflect_shader_source(const std::string& glsl, std::vector<ReflectedUbo>& ubos,
                           std::vector<ReflectedSampler>& samplers) {
    std::istringstream iss(glsl);
    std::string line;
    while (std::getline(iss, line)) {
        const size_t pos = line.find("layout(set = 0, binding = ");
        if (pos == std::string::npos) continue;
        const char* p = line.c_str() + pos + 26;
        const uint32_t binding = static_cast<uint32_t>(std::strtoul(p, nullptr, 10));
        const size_t std140 = line.find(", std140) uniform ");
        if (std140 != std::string::npos) {
            const size_t name_start = std140 + 18;
            const size_t name_end = line.find(' ', name_start);
            if (name_end != std::string::npos) {
                const std::string block = line.substr(name_start, name_end - name_start);
                bool found = false;
                for (const auto& u : ubos) {
                    if (u.binding == binding) { found = true; break; }
                }
                if (!found) ubos.push_back({binding, block, binding});
            }
            continue;
        }
        const size_t uni = line.find(") uniform ");
        if (uni != std::string::npos) {
            const size_t type_start = uni + 10;
            const size_t type_end = line.find(' ', type_start);
            const size_t semi = line.find(';', type_end);
            if (type_end != std::string::npos && semi != std::string::npos) {
                const std::string type = line.substr(type_start, type_end - type_start);
                const std::string name = line.substr(type_end + 1, semi - (type_end + 1));
                const bool is_cube = type.find("Cube") != std::string::npos;
                const bool is_array = type.find("Array") != std::string::npos;
                const bool is_shadow = type.find("Shadow") != std::string::npos;
                bool found = false;
                for (const auto& s : samplers) {
                    if (s.binding == binding) { found = true; break; }
                }
                if (!found) samplers.push_back({binding, name, is_cube, is_array, is_shadow, 0});
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Vulkan GPU Objects
// -----------------------------------------------------------------------------
struct VkBufObj {
    GLenum last_usage = GL_STATIC_DRAW;
    size_t size = 0;
    VkBuffer static_buf = VK_NULL_HANDLE;
    VkDeviceMemory static_mem = VK_NULL_HANDLE;
    void* static_mapped = nullptr;

    // Active sub-allocation (either static_buf + 0 or ring_buf + ring_offset)
    VkBuffer active_buf = VK_NULL_HANDLE;
    VkDeviceSize active_offset = 0;
    size_t active_size = 0;
    uint8_t* active_cpu = nullptr;
    std::vector<uint8_t> shadow_copy;  // kept for small UBO partial updates via glBufferSubData
};

struct VkTexObj {
    GLenum target = GL_TEXTURE_2D;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView sample_view = VK_NULL_HANDLE;
    std::vector<VkImageView> layer_views;  // for shadow 2D array slice rendering
    VkFormat format = VK_FORMAT_UNDEFINED;
    int width = 0;
    int height = 0;
    int layers = 1;
    uint32_t mip_levels = 1;
    uint32_t valid_mips = 1;
    int max_level_param = 1000;
    bool is_depth = false;
    VkComponentMapping swizzle = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
    };
    // Exact layout per (mip * layers + layer)
    std::vector<VkImageLayout> subresource_layouts;

    VkImageLayout& layout_at(uint32_t mip, uint32_t layer) {
        return subresource_layouts[mip * static_cast<uint32_t>(layers) + layer];
    }
};

struct VkSamplerObj {
    VkSampler sampler = VK_NULL_HANDLE;
    GLint min_filter = GL_LINEAR;
    GLint mag_filter = GL_LINEAR;
    GLint wrap_s = GL_REPEAT;
    GLint wrap_t = GL_REPEAT;
    GLint wrap_r = GL_REPEAT;
    GLint compare_mode = GL_NONE;
    GLint compare_func = GL_LEQUAL;
    float min_lod = 0.0f;
    float max_lod = 16.0f;
    float max_aniso = 1.0f;
    bool dirty = true;
};

struct VkAttrib {
    bool enabled = false;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    bool normalized = false;
    bool integer = false;
    GLsizei stride = 0;
    uint32_t offset = 0;
    GLuint vbo = 0;
};

struct VkVaoObj {
    std::array<VkAttrib, 16> attribs{};
};

struct VkShaderObj {
    GLenum type = GL_VERTEX_SHADER;
    std::string source;
    std::string log;
    std::vector<uint32_t> spirv;
    VkShaderModule module = VK_NULL_HANDLE;
    bool compiled = false;
};

struct VkProgramObj {
    GLuint vs_id = 0;
    GLuint fs_id = 0;
    VkShaderModule vs_module = VK_NULL_HANDLE;
    VkShaderModule fs_module = VK_NULL_HANDLE;
    bool linked = false;
    std::string log;
    std::vector<ReflectedUbo> ubos;
    std::vector<ReflectedSampler> samplers;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
};

struct VkFboObj {
    GLuint color_tex = 0;
    GLint color_level = 0;
    GLint color_layer = 0;
    GLuint depth_tex = 0;
    GLint depth_level = 0;
    GLint depth_layer = 0;
};

struct RenderPassKey {
    VkFormat color_fmt = VK_FORMAT_UNDEFINED;
    VkFormat depth_fmt = VK_FORMAT_UNDEFINED;
    uint8_t color_load = 0;
    uint8_t depth_load = 0;
    bool operator==(const RenderPassKey& o) const {
        return color_fmt == o.color_fmt && depth_fmt == o.depth_fmt &&
               color_load == o.color_load && depth_load == o.depth_load;
    }
};

struct RenderPassKeyHash {
    size_t operator()(const RenderPassKey& k) const {
        return (static_cast<size_t>(k.color_fmt) * 1315423911u) ^
               (static_cast<size_t>(k.depth_fmt) * 2654435761u) ^
               (static_cast<size_t>(k.color_load) << 8) ^ k.depth_load;
    }
};

struct FbInstanceKey {
    VkRenderPass rp = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    uint32_t w = 0;
    uint32_t h = 0;
    bool operator==(const FbInstanceKey& o) const {
        return rp == o.rp && color_view == o.color_view && depth_view == o.depth_view && w == o.w && h == o.h;
    }
};

struct FbInstanceKeyHash {
    size_t operator()(const FbInstanceKey& k) const {
        size_t h = reinterpret_cast<uintptr_t>(k.rp);
        h ^= reinterpret_cast<uintptr_t>(k.color_view) * 1315423911u;
        h ^= reinterpret_cast<uintptr_t>(k.depth_view) * 2654435761u;
        h ^= (static_cast<size_t>(k.w) << 16) ^ k.h;
        return h;
    }
};

struct PipelineKey {
    GLuint program = 0;
    VkRenderPass rp = VK_NULL_HANDLE;
    bool has_color_att = false;
    GLenum topology = GL_TRIANGLES;
    bool blend_enable = false;
    GLenum blend_src_rgb = GL_ONE;
    GLenum blend_dst_rgb = GL_ZERO;
    GLenum blend_src_a = GL_ONE;
    GLenum blend_dst_a = GL_ZERO;
    GLenum blend_eq = GL_FUNC_ADD;
    bool cull_enable = false;
    GLenum cull_face = GL_BACK;
    GLenum front_face = GL_CCW;
    bool depth_test = false;
    bool depth_write = true;
    GLenum depth_func = GL_LEQUAL;
    uint64_t vao_hash = 0;

    bool operator==(const PipelineKey& o) const {
        return std::memcmp(this, &o, sizeof(PipelineKey)) == 0;
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&k);
        uint64_t h = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < sizeof(PipelineKey); ++i) {
            h ^= bytes[i];
            h *= 0x100000001b3ULL;
        }
        return static_cast<size_t>(h);
    }
};

// -----------------------------------------------------------------------------
// Global Vulkan Context State
// -----------------------------------------------------------------------------
struct VkContext {
    VkDispatch vk;
    SpirvCache spirv_cache;
    bool initialized = false;
    bool headless = false;
    SDL_Window* sdl_window = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys_dev = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;

    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceFeatures enabled_features{};
    VkPhysicalDeviceMemoryProperties mem_props{};
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkFormat r32f_format = VK_FORMAT_R32_SFLOAT;
    bool supports_bc = false;

    std::string renderer_str;
    std::string version_str;
    std::string vendor_str;

    // Swapchain
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swap_extent{0, 0};
    std::vector<VkImage> swap_images;
    std::vector<VkImageLayout> swap_layouts;
    VkSemaphore acquire_sem = VK_NULL_HANDLE;
    std::vector<VkSemaphore> render_done_sems;
    uint32_t current_swap_image = 0;
    bool has_acquired_image = false;

    // Command pools & fences
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer frame_cmd = VK_NULL_HANDLE;
    VkFence frame_fence = VK_NULL_HANDLE;
    VkCommandBuffer upload_cmd = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;
    bool upload_recording = false;
    bool in_frame = false;

    // Persistently mapped dynamic ring buffer (64 MB) for per-draw UBOs, VBOs, IBOs & dynamic textures
    static constexpr size_t kRingSize = 64 * 1024 * 1024;
    VkBuffer ring_buf = VK_NULL_HANDLE;
    VkDeviceMemory ring_mem = VK_NULL_HANDLE;
    uint8_t* ring_ptr = nullptr;
    size_t ring_offset = 0;

    // Upload staging buffer (64 MB) for static asset textures
    static constexpr size_t kStagingSize = 64 * 1024 * 1024;
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    uint8_t* staging_ptr = nullptr;
    size_t staging_offset = 0;
    struct TransientStagingChunk {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint8_t* ptr = nullptr;
        size_t offset = 0;
        size_t cap = 0;
    };
    std::vector<TransientStagingChunk> transient_staging;

    // Descriptor pool & pipeline cache
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkPipelineCache pipe_cache = VK_NULL_HANDLE;
    std::string pipe_cache_file;

    // Dummy fallback descriptors (guarantees zero unbound descriptors on any shader)
    VkTexObj dummy_white_2d;
    VkTexObj dummy_cube;
    VkTexObj dummy_depth_2d;
    VkTexObj dummy_depth_array;
    VkSampler dummy_linear_sampler = VK_NULL_HANDLE;
    VkSampler dummy_shadow_sampler = VK_NULL_HANDLE;
    VkBuffer dummy_ubo_buf = VK_NULL_HANDLE;
    VkDeviceMemory dummy_ubo_mem = VK_NULL_HANDLE;

    // Handle tables (1-indexed)
    GLuint next_id = 1;
    std::unordered_map<GLuint, VkBufObj> buffers;
    std::unordered_map<GLuint, VkTexObj> textures;
    std::unordered_map<GLuint, VkSamplerObj> samplers;
    std::unordered_map<GLuint, VkVaoObj> vaos;
    std::unordered_map<GLuint, VkShaderObj> shaders;
    std::unordered_map<GLuint, VkProgramObj> programs;
    std::unordered_map<GLuint, VkFboObj> fbos;

    std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHash> render_passes;
    std::unordered_map<FbInstanceKey, VkFramebuffer, FbInstanceKeyHash> framebuffers;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;

    // State machine
    GLuint active_tex_unit = 0;
    std::array<GLuint, 64> tex_units{};
    std::array<GLuint, 64> smp_units{};
    GLuint bound_array_buf = 0;
    GLuint bound_element_buf = 0;
    GLuint bound_uniform_buf = 0;
    GLuint bound_vao = 0;
    GLuint current_program = 0;
    GLuint draw_fbo = 0;
    GLuint read_fbo = 0;
    GLuint last_present_candidate_tex = 0;

    struct UboSlot {
        GLuint buf = 0;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
    };
    std::array<UboSlot, 64> ubo_slots{};

    bool blend_enable = false;
    GLenum blend_src_rgb = GL_ONE;
    GLenum blend_dst_rgb = GL_ZERO;
    GLenum blend_src_a = GL_ONE;
    GLenum blend_dst_a = GL_ZERO;
    GLenum blend_eq = GL_FUNC_ADD;
    bool cull_enable = false;
    GLenum cull_face = GL_BACK;
    GLenum front_face = GL_CCW;
    bool depth_test = false;
    bool depth_write = true;
    GLenum depth_func = GL_LEQUAL;
    bool poly_offset_enable = false;
    float poly_factor = 0.0f;
    float poly_units = 0.0f;
    float poly_clamp = 0.0f;

    int vp_x = 0, vp_y = 0, vp_w = 1280, vp_h = 720;
    float depth_near = 0.0f, depth_far = 1.0f;
    float clear_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float clear_depth_val = 1.0f;

    // Active render pass tracking
    bool rp_active = false;
    GLuint rp_fbo_id = 0;
    VkRenderPass current_rp = VK_NULL_HANDLE;
    bool current_rp_has_color = false;
    uint32_t current_fb_w = 0;
    uint32_t current_fb_h = 0;
    GLuint current_color_tex_id = 0;
    GLuint current_depth_tex_id = 0;
};

VkContext g_ctx;

uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < g_ctx.mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (g_ctx.mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    for (uint32_t i = 0; i < g_ctx.mem_props.memoryTypeCount; ++i) {
        if (type_filter & (1u << i)) return i;
    }
    return 0;
}

bool create_vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                      VkBuffer& buf, VkDeviceMemory& mem, void** mapped = nullptr) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = std::max<VkDeviceSize>(size, 64);
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_ctx.vk.CreateBuffer(g_ctx.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    g_ctx.vk.GetBufferMemoryRequirements(g_ctx.device, buf, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, props);
    if (g_ctx.vk.AllocateMemory(g_ctx.device, &mai, nullptr, &mem) != VK_SUCCESS) {
        g_ctx.vk.DestroyBuffer(g_ctx.device, buf, nullptr);
        buf = VK_NULL_HANDLE;
        return false;
    }
    g_ctx.vk.BindBufferMemory(g_ctx.device, buf, mem, 0);
    if (mapped) g_ctx.vk.MapMemory(g_ctx.device, mem, 0, VK_WHOLE_SIZE, 0, mapped);
    return true;
}

VkCommandBuffer get_upload_cmd() {
    if (g_ctx.in_frame) return g_ctx.frame_cmd;
    if (!g_ctx.upload_recording) {
        g_ctx.vk.ResetCommandBuffer(g_ctx.upload_cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_ctx.vk.BeginCommandBuffer(g_ctx.upload_cmd, &bi);
        g_ctx.upload_recording = true;
    }
    return g_ctx.upload_cmd;
}

void flush_upload_cmd() {
    if (!g_ctx.upload_recording) return;
    g_ctx.vk.EndCommandBuffer(g_ctx.upload_cmd);
    g_ctx.vk.ResetFences(g_ctx.device, 1, &g_ctx.upload_fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_ctx.upload_cmd;
    g_ctx.vk.QueueSubmit(g_ctx.queue, 1, &si, g_ctx.upload_fence);
    g_ctx.vk.WaitForFences(g_ctx.device, 1, &g_ctx.upload_fence, VK_TRUE, UINT64_MAX);
    g_ctx.upload_recording = false;
    g_ctx.staging_offset = 0;
}

void end_active_render_pass() {
    if (!g_ctx.rp_active) return;
    g_ctx.vk.CmdEndRenderPass(g_ctx.frame_cmd);
    g_ctx.rp_active = false;
    g_ctx.rp_fbo_id = 0;
    g_ctx.current_color_tex_id = 0;
    g_ctx.current_depth_tex_id = 0;
}

void transition_subresource(VkCommandBuffer cmd, VkTexObj& tex, uint32_t mip, uint32_t layer,
                            VkImageLayout new_layout) {
    VkImageLayout& cur = tex.layout_at(mip, layer);
    if (cur == new_layout) return;

    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = cur;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex.image;
    b.subresourceRange.aspectMask = tex.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = mip;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = layer;
    b.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    switch (cur) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            b.srcAccessMask = 0;
            src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        default:
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
            break;
    }

    switch (new_layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        default:
            b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
            break;
    }

    g_ctx.vk.CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    cur = new_layout;
}

void transition_all_subresources(VkCommandBuffer cmd, VkTexObj& tex, VkImageLayout new_layout) {
    for (uint32_t m = 0; m < tex.mip_levels; ++m) {
        for (int l = 0; l < tex.layers; ++l) {
            transition_subresource(cmd, tex, m, static_cast<uint32_t>(l), new_layout);
        }
    }
}

void update_tex_sample_view(VkTexObj& tex) {
    if (tex.sample_view != VK_NULL_HANDLE) {
        g_ctx.vk.DestroyImageView(g_ctx.device, tex.sample_view, nullptr);
        tex.sample_view = VK_NULL_HANDLE;
    }
    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = tex.image;
    ivci.viewType = (tex.target == GL_TEXTURE_CUBE_MAP)
                        ? VK_IMAGE_VIEW_TYPE_CUBE
                        : ((tex.target == GL_TEXTURE_2D_ARRAY) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    ivci.format = tex.format;
    ivci.components = tex.swizzle;
    ivci.subresourceRange.aspectMask = tex.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.baseMipLevel = 0;
    const uint32_t allowed = std::min<uint32_t>(tex.mip_levels, static_cast<uint32_t>(std::max(1, tex.max_level_param + 1)));
    ivci.subresourceRange.levelCount = std::max<uint32_t>(1u, std::min<uint32_t>(tex.valid_mips, allowed));
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount = static_cast<uint32_t>(tex.layers);
    g_ctx.vk.CreateImageView(g_ctx.device, &ivci, nullptr, &tex.sample_view);
}

bool allocate_vk_image(VkTexObj& tex, int width, int height, int layers, uint32_t mip_levels,
                       VkFormat fmt, bool is_depth, VkImageUsageFlags extra_usage = 0) {
    if (tex.sample_view) g_ctx.vk.DestroyImageView(g_ctx.device, tex.sample_view, nullptr);
    for (VkImageView v : tex.layer_views) {
        if (v) g_ctx.vk.DestroyImageView(g_ctx.device, v, nullptr);
    }
    tex.layer_views.clear();
    if (tex.image) g_ctx.vk.DestroyImage(g_ctx.device, tex.image, nullptr);
    if (tex.memory) g_ctx.vk.FreeMemory(g_ctx.device, tex.memory, nullptr);

    tex.width = std::max(1, width);
    tex.height = std::max(1, height);
    tex.layers = std::max(1, layers);
    tex.mip_levels = std::max<uint32_t>(1u, mip_levels);
    tex.valid_mips = 1;
    tex.format = fmt;
    tex.is_depth = is_depth;
    tex.subresource_layouts.assign(tex.mip_levels * static_cast<uint32_t>(tex.layers), VK_IMAGE_LAYOUT_UNDEFINED);

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.flags = (tex.target == GL_TEXTURE_CUBE_MAP) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {static_cast<uint32_t>(tex.width), static_cast<uint32_t>(tex.height), 1};
    ici.mipLevels = tex.mip_levels;
    ici.arrayLayers = static_cast<uint32_t>(tex.layers);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | extra_usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (g_ctx.vk.CreateImage(g_ctx.device, &ici, nullptr, &tex.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    g_ctx.vk.GetImageMemoryRequirements(g_ctx.device, tex.image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (g_ctx.vk.AllocateMemory(g_ctx.device, &mai, nullptr, &tex.memory) != VK_SUCCESS) {
        g_ctx.vk.DestroyImage(g_ctx.device, tex.image, nullptr);
        tex.image = VK_NULL_HANDLE;
        return false;
    }
    g_ctx.vk.BindImageMemory(g_ctx.device, tex.image, tex.memory, 0);

    update_tex_sample_view(tex);
    tex.layer_views.resize(static_cast<size_t>(tex.layers), VK_NULL_HANDLE);
    for (int l = 0; l < tex.layers; ++l) {
        VkImageViewCreateInfo lv{};
        lv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        lv.image = tex.image;
        lv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        lv.format = fmt;
        lv.subresourceRange.aspectMask = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        lv.subresourceRange.baseMipLevel = 0;
        lv.subresourceRange.levelCount = 1;
        lv.subresourceRange.baseArrayLayer = static_cast<uint32_t>(l);
        lv.subresourceRange.layerCount = 1;
        g_ctx.vk.CreateImageView(g_ctx.device, &lv, nullptr, &tex.layer_views[static_cast<size_t>(l)]);
    }
    return true;
}

uint8_t* alloc_staging_or_ring(size_t bytes, VkBuffer& out_buf, VkDeviceSize& out_offset) {
    bytes = std::max<size_t>(bytes, 16);
    if (g_ctx.in_frame) {
        const size_t align = std::max<size_t>(256, g_ctx.props.limits.minUniformBufferOffsetAlignment);
        const size_t aligned = (g_ctx.ring_offset + align - 1) & ~(align - 1);
        if (aligned + bytes <= VkContext::kRingSize) {
            out_buf = g_ctx.ring_buf;
            out_offset = aligned;
            uint8_t* p = g_ctx.ring_ptr + aligned;
            g_ctx.ring_offset = aligned + bytes;
            return p;
        }
        const size_t stg_aligned = (g_ctx.staging_offset + 15) & ~size_t(15);
        if (stg_aligned + bytes <= VkContext::kStagingSize) {
            out_buf = g_ctx.staging_buf;
            out_offset = stg_aligned;
            uint8_t* p = g_ctx.staging_ptr + stg_aligned;
            g_ctx.staging_offset = stg_aligned + bytes;
            return p;
        }
        if (g_ctx.transient_staging.empty() ||
            ((g_ctx.transient_staging.back().offset + 15) & ~size_t(15)) + bytes > g_ctx.transient_staging.back().cap) {
            VkContext::TransientStagingChunk chunk{};
            chunk.cap = std::max<size_t>(VkContext::kStagingSize, bytes + 4096);
            void* mapped = nullptr;
            create_vk_buffer(chunk.cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             chunk.buf, chunk.mem, &mapped);
            chunk.ptr = static_cast<uint8_t*>(mapped);
            g_ctx.transient_staging.push_back(chunk);
        }
        auto& c = g_ctx.transient_staging.back();
        const size_t ca = (c.offset + 15) & ~size_t(15);
        out_buf = c.buf;
        out_offset = ca;
        uint8_t* p = c.ptr + ca;
        c.offset = ca + bytes;
        return p;
    }
    const size_t align = 16;
    size_t aligned = (g_ctx.staging_offset + align - 1) & ~(align - 1);
    if (aligned + bytes > VkContext::kStagingSize) {
        flush_upload_cmd();
        aligned = 0;
    }
    out_buf = g_ctx.staging_buf;
    out_offset = aligned;
    uint8_t* p = g_ctx.staging_ptr + aligned;
    g_ctx.staging_offset = aligned + bytes;
    return p;
}

void upload_mip_bytes(VkTexObj& tex, uint32_t mip, uint32_t layer, int w, int h, const void* data, size_t byte_size) {
    end_active_render_pass();
    VkBuffer src_buf = VK_NULL_HANDLE;
    VkDeviceSize src_off = 0;
    uint8_t* dst = alloc_staging_or_ring(byte_size, src_buf, src_off);
    std::memcpy(dst, data, byte_size);

    VkCommandBuffer cmd = get_upload_cmd();
    transition_subresource(cmd, tex, mip, layer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.bufferOffset = src_off;
    region.imageSubresource.aspectMask = tex.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mip;
    region.imageSubresource.baseArrayLayer = layer;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(std::max(1, w)), static_cast<uint32_t>(std::max(1, h)), 1};
    g_ctx.vk.CmdCopyBufferToImage(cmd, src_buf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition_subresource(cmd, tex, mip, layer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (mip + 1 > tex.valid_mips) {
        tex.valid_mips = mip + 1;
        update_tex_sample_view(tex);
    }
}

VkRenderPass get_or_create_render_pass(const RenderPassKey& key) {
    const auto it = g_ctx.render_passes.find(key);
    if (it != g_ctx.render_passes.end()) return it->second;

    std::vector<VkAttachmentDescription> atts;
    VkAttachmentReference color_ref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    if (key.color_fmt != VK_FORMAT_UNDEFINED) {
        VkAttachmentDescription c{};
        c.format = key.color_fmt;
        c.samples = VK_SAMPLE_COUNT_1_BIT;
        c.loadOp = (key.color_load == 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        c.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        c.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        c.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        c.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_ref.attachment = static_cast<uint32_t>(atts.size());
        atts.push_back(c);
    }
    if (key.depth_fmt != VK_FORMAT_UNDEFINED) {
        VkAttachmentDescription d{};
        d.format = key.depth_fmt;
        d.samples = VK_SAMPLE_COUNT_1_BIT;
        d.loadOp = (key.depth_load == 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        d.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        d.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref.attachment = static_cast<uint32_t>(atts.size());
        atts.push_back(d);
    }

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (color_ref.attachment != VK_ATTACHMENT_UNUSED) {
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &color_ref;
    }
    if (depth_ref.attachment != VK_ATTACHMENT_UNUSED) {
        sub.pDepthStencilAttachment = &depth_ref;
    }

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = static_cast<uint32_t>(atts.size());
    rpci.pAttachments = atts.data();
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;

    VkRenderPass rp = VK_NULL_HANDLE;
    g_ctx.vk.CreateRenderPass(g_ctx.device, &rpci, nullptr, &rp);
    g_ctx.render_passes[key] = rp;
    return rp;
}

VkFramebuffer get_or_create_framebuffer(const FbInstanceKey& key) {
    const auto it = g_ctx.framebuffers.find(key);
    if (it != g_ctx.framebuffers.end()) return it->second;

    std::vector<VkImageView> views;
    if (key.color_view != VK_NULL_HANDLE) views.push_back(key.color_view);
    if (key.depth_view != VK_NULL_HANDLE) views.push_back(key.depth_view);

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = key.rp;
    fbci.attachmentCount = static_cast<uint32_t>(views.size());
    fbci.pAttachments = views.data();
    fbci.width = key.w;
    fbci.height = key.h;
    fbci.layers = 1;

    VkFramebuffer fb = VK_NULL_HANDLE;
    g_ctx.vk.CreateFramebuffer(g_ctx.device, &fbci, nullptr, &fb);
    g_ctx.framebuffers[key] = fb;
    return fb;
}

void ensure_render_pass_open(bool clear_col, const float* col_rgba, bool clear_dep, float dep_val) {
    if (g_ctx.draw_fbo == 0) return;
    const auto fbo_it = g_ctx.fbos.find(g_ctx.draw_fbo);
    if (fbo_it == g_ctx.fbos.end()) return;
    const VkFboObj& fbo = fbo_it->second;

    if (g_ctx.rp_active && g_ctx.rp_fbo_id == g_ctx.draw_fbo) {
        if (clear_col || clear_dep) {
            std::vector<VkClearAttachment> clears;
            if (clear_col && fbo.color_tex != 0) {
                VkClearAttachment ca{};
                ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ca.colorAttachment = 0;
                std::memcpy(ca.clearValue.color.float32, col_rgba, 16);
                clears.push_back(ca);
            }
            if (clear_dep && fbo.depth_tex != 0) {
                VkClearAttachment da{};
                da.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                da.clearValue.depthStencil.depth = dep_val;
                clears.push_back(da);
            }
            if (!clears.empty()) {
                VkClearRect rect{};
                rect.rect.offset = {0, 0};
                rect.rect.extent = {g_ctx.current_fb_w, g_ctx.current_fb_h};
                rect.baseArrayLayer = 0;
                rect.layerCount = 1;
                g_ctx.vk.CmdClearAttachments(g_ctx.frame_cmd, static_cast<uint32_t>(clears.size()), clears.data(), 1, &rect);
            }
        }
        return;
    }

    end_active_render_pass();

    VkTexObj* c_tex = fbo.color_tex ? &g_ctx.textures[fbo.color_tex] : nullptr;
    VkTexObj* d_tex = fbo.depth_tex ? &g_ctx.textures[fbo.depth_tex] : nullptr;
    if (!c_tex && !d_tex) return;

    VkCommandBuffer cmd = get_upload_cmd();
    bool c_was_undef = false;
    bool d_was_undef = false;

    if (c_tex) {
        c_was_undef = (c_tex->layout_at(0, static_cast<uint32_t>(fbo.color_layer)) == VK_IMAGE_LAYOUT_UNDEFINED);
        transition_subresource(cmd, *c_tex, 0, static_cast<uint32_t>(fbo.color_layer), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (c_tex->format == VK_FORMAT_R8G8B8A8_UNORM && !d_tex && c_tex->width >= 320 && c_tex->height >= 240) {
            g_ctx.last_present_candidate_tex = fbo.color_tex;
        }
    }
    if (d_tex) {
        d_was_undef = (d_tex->layout_at(0, static_cast<uint32_t>(fbo.depth_layer)) == VK_IMAGE_LAYOUT_UNDEFINED);
        transition_subresource(cmd, *d_tex, 0, static_cast<uint32_t>(fbo.depth_layer), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    RenderPassKey rpk{};
    rpk.color_fmt = c_tex ? c_tex->format : VK_FORMAT_UNDEFINED;
    rpk.depth_fmt = d_tex ? d_tex->format : VK_FORMAT_UNDEFINED;
    rpk.color_load = (clear_col || c_was_undef) ? 1 : 0;
    rpk.depth_load = (clear_dep || d_was_undef) ? 1 : 0;

    VkRenderPass rp = get_or_create_render_pass(rpk);
    const uint32_t fb_w = static_cast<uint32_t>(c_tex ? c_tex->width : d_tex->width);
    const uint32_t fb_h = static_cast<uint32_t>(c_tex ? c_tex->height : d_tex->height);

    FbInstanceKey fbk{};
    fbk.rp = rp;
    fbk.color_view = c_tex ? c_tex->layer_views[static_cast<size_t>(fbo.color_layer)] : VK_NULL_HANDLE;
    fbk.depth_view = d_tex ? d_tex->layer_views[static_cast<size_t>(fbo.depth_layer)] : VK_NULL_HANDLE;
    fbk.w = fb_w;
    fbk.h = fb_h;
    VkFramebuffer fb = get_or_create_framebuffer(fbk);

    std::vector<VkClearValue> cvs;
    if (c_tex) {
        VkClearValue cv{};
        if (col_rgba) std::memcpy(cv.color.float32, col_rgba, 16);
        cvs.push_back(cv);
    }
    if (d_tex) {
        VkClearValue dv{};
        dv.depthStencil.depth = clear_dep ? dep_val : 1.0f;
        cvs.push_back(dv);
    }

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = {fb_w, fb_h};
    rpbi.clearValueCount = static_cast<uint32_t>(cvs.size());
    rpbi.pClearValues = cvs.data();
    g_ctx.vk.CmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    g_ctx.rp_active = true;
    g_ctx.rp_fbo_id = g_ctx.draw_fbo;
    g_ctx.current_rp = rp;
    g_ctx.current_rp_has_color = (c_tex != nullptr);
    g_ctx.current_fb_w = fb_w;
    g_ctx.current_fb_h = fb_h;
    g_ctx.current_color_tex_id = fbo.color_tex;
    g_ctx.current_depth_tex_id = fbo.depth_tex;
}

VkSampler ensure_vk_sampler(VkSamplerObj& s) {
    if (!s.dirty && s.sampler != VK_NULL_HANDLE) return s.sampler;
    if (s.sampler != VK_NULL_HANDLE) g_ctx.vk.DestroySampler(g_ctx.device, s.sampler, nullptr);

    auto addr = [](GLint w) -> VkSamplerAddressMode {
        if (w == GL_CLAMP_TO_EDGE) return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (w == GL_MIRRORED_REPEAT) return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = (s.mag_filter == GL_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.minFilter = (s.min_filter == GL_NEAREST || s.min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                     s.min_filter == GL_NEAREST_MIPMAP_LINEAR)
                        ? VK_FILTER_NEAREST
                        : VK_FILTER_LINEAR;
    sci.mipmapMode = (s.min_filter == GL_NEAREST_MIPMAP_NEAREST || s.min_filter == GL_LINEAR_MIPMAP_NEAREST)
                         ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                         : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = addr(s.wrap_s);
    sci.addressModeV = addr(s.wrap_t);
    sci.addressModeW = addr(s.wrap_r);
    sci.minLod = s.min_lod;
    sci.maxLod = s.max_lod;
    if (s.max_aniso > 1.0f && g_ctx.enabled_features.samplerAnisotropy) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = std::min(s.max_aniso, g_ctx.props.limits.maxSamplerAnisotropy);
    }
    if (s.compare_mode == GL_COMPARE_REF_TO_TEXTURE) {
        sci.compareEnable = VK_TRUE;
        sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    }
    g_ctx.vk.CreateSampler(g_ctx.device, &sci, nullptr, &s.sampler);
    s.dirty = false;
    return s.sampler;
}

VkFormat vertex_attrib_format(const VkAttrib& a) {
    if (a.integer || a.type == GL_UNSIGNED_INT) {
        if (a.size == 1) return VK_FORMAT_R32_UINT;
        if (a.size == 2) return VK_FORMAT_R32G32_UINT;
        if (a.size == 3) return VK_FORMAT_R32G32B32_UINT;
        return VK_FORMAT_R32G32B32A32_UINT;
    }
    if (a.type == GL_UNSIGNED_BYTE) {
        return a.normalized ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_UINT;
    }
    if (a.size == 1) return VK_FORMAT_R32_SFLOAT;
    if (a.size == 2) return VK_FORMAT_R32G32_SFLOAT;
    if (a.size == 3) return VK_FORMAT_R32G32B32_SFLOAT;
    return VK_FORMAT_R32G32B32A32_SFLOAT;
}

VkBlendFactor map_blend_factor(GLenum f) {
    switch (f) {
        case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
        case GL_ONE: return VK_BLEND_FACTOR_ONE;
        case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
        case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
        case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
        case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkCompareOp map_compare_op(GLenum f) {
    switch (f) {
        case GL_NEVER: return VK_COMPARE_OP_NEVER;
        case GL_LESS: return VK_COMPARE_OP_LESS;
        case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
        case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case GL_GREATER: return VK_COMPARE_OP_GREATER;
        case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
    }
}

bool prepare_draw(VkCommandBuffer& out_cmd, uint32_t& out_vertex_binding_stride) {
    if (g_ctx.draw_fbo == 0 || g_ctx.current_program == 0) return false;
    auto prog_it = g_ctx.programs.find(g_ctx.current_program);
    if (prog_it == g_ctx.programs.end() || !prog_it->second.linked) return false;
    VkProgramObj& prog = prog_it->second;

    const auto fbo_it = g_ctx.fbos.find(g_ctx.draw_fbo);
    if (fbo_it == g_ctx.fbos.end()) return false;
    const VkFboObj& fbo = fbo_it->second;

    // Ensure all sampled textures that are NOT attached to draw_fbo are in SHADER_READ_ONLY_OPTIMAL BEFORE render pass begins!
    for (const ReflectedSampler& s : prog.samplers) {
        const GLuint unit = static_cast<GLuint>(std::clamp(s.bound_unit, 0, 63));
        const GLuint tid = g_ctx.tex_units[unit];
        if (tid == 0 || tid == fbo.color_tex || tid == fbo.depth_tex) continue;
        auto tit = g_ctx.textures.find(tid);
        if (tit == g_ctx.textures.end()) continue;
        VkTexObj& tex = tit->second;
        bool needs_barrier = false;
        for (VkImageLayout l : tex.subresource_layouts) {
            if (l != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                needs_barrier = true;
                break;
            }
        }
        if (needs_barrier) {
            end_active_render_pass();
            transition_all_subresources(get_upload_cmd(), tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    ensure_render_pass_open(false, nullptr, false, 1.0f);
    out_cmd = get_upload_cmd();

    // Vertex input state
    const VkVaoObj* vao = g_ctx.bound_vao ? &g_ctx.vaos[g_ctx.bound_vao] : nullptr;
    uint32_t stride = 0;
    uint64_t vao_hash = 0xcbf29ce484222325ULL;
    std::vector<VkVertexInputAttributeDescription> vk_attribs;
    if (vao) {
        for (uint32_t i = 0; i < 16; ++i) {
            const VkAttrib& a = vao->attribs[i];
            if (!a.enabled) continue;
            stride = a.stride;
            VkVertexInputAttributeDescription ad{};
            ad.location = i;
            ad.binding = 0;
            ad.format = vertex_attrib_format(a);
            ad.offset = a.offset;
            vk_attribs.push_back(ad);
            vao_hash ^= (static_cast<uint64_t>(ad.location) << 40) ^ (static_cast<uint64_t>(ad.format) << 16) ^ ad.offset;
            vao_hash *= 0x100000001b3ULL;
        }
        vao_hash ^= stride;
    }
    out_vertex_binding_stride = stride;

    PipelineKey pk{};
    pk.program = g_ctx.current_program;
    pk.rp = g_ctx.current_rp;
    pk.has_color_att = g_ctx.current_rp_has_color;
    pk.topology = GL_TRIANGLES;
    pk.blend_enable = g_ctx.blend_enable;
    pk.blend_src_rgb = g_ctx.blend_src_rgb;
    pk.blend_dst_rgb = g_ctx.blend_dst_rgb;
    pk.blend_src_a = g_ctx.blend_src_a;
    pk.blend_dst_a = g_ctx.blend_dst_a;
    pk.blend_eq = g_ctx.blend_eq;
    pk.cull_enable = g_ctx.cull_enable;
    pk.cull_face = g_ctx.cull_face;
    pk.front_face = g_ctx.front_face;
    pk.depth_test = g_ctx.depth_test;
    pk.depth_write = g_ctx.depth_write;
    pk.depth_func = g_ctx.depth_func;
    pk.vao_hash = vao_hash;

    VkPipeline pipe = VK_NULL_HANDLE;
    const auto pit = g_ctx.pipelines.find(pk);
    if (pit != g_ctx.pipelines.end()) {
        pipe = pit->second;
    } else {
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        if (prog.vs_module) {
            VkPipelineShaderStageCreateInfo s{};
            s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            s.stage = VK_SHADER_STAGE_VERTEX_BIT;
            s.module = prog.vs_module;
            s.pName = "main";
            stages.push_back(s);
        }
        if (prog.fs_module) {
            VkPipelineShaderStageCreateInfo s{};
            s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            s.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            s.module = prog.fs_module;
            s.pName = "main";
            stages.push_back(s);
        }

        VkVertexInputBindingDescription bind_desc{};
        bind_desc.binding = 0;
        bind_desc.stride = stride;
        bind_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if (!vk_attribs.empty()) {
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &bind_desc;
            vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(vk_attribs.size());
            vi.pVertexAttributeDescriptions = vk_attribs.data();
        }

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = pk.cull_enable ? ((pk.cull_face == GL_FRONT) ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT)
                                     : VK_CULL_MODE_NONE;
        rs.frontFace = (pk.front_face == GL_CW) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.depthBiasEnable = VK_TRUE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = pk.depth_test ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = pk.depth_write ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = map_compare_op(pk.depth_func);

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable = pk.blend_enable ? VK_TRUE : VK_FALSE;
        cba.srcColorBlendFactor = map_blend_factor(pk.blend_src_rgb);
        cba.dstColorBlendFactor = map_blend_factor(pk.blend_dst_rgb);
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = map_blend_factor(pk.blend_src_a);
        cba.dstAlphaBlendFactor = map_blend_factor(pk.blend_dst_a);
        cba.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        if (pk.has_color_att) {
            cb.attachmentCount = 1;
            cb.pAttachments = &cba;
        }

        const VkDynamicState dyn_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
        };
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 3;
        dyn.pDynamicStates = dyn_states;

        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = static_cast<uint32_t>(stages.size());
        gpci.pStages = stages.data();
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vps;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pDepthStencilState = &ds;
        gpci.pColorBlendState = &cb;
        gpci.pDynamicState = &dyn;
        gpci.layout = prog.pipe_layout;
        gpci.renderPass = pk.rp;
        gpci.subpass = 0;

        g_ctx.vk.CreateGraphicsPipelines(g_ctx.device, g_ctx.pipe_cache, 1, &gpci, nullptr, &pipe);
        g_ctx.pipelines[pk] = pipe;
    }

    g_ctx.vk.CmdBindPipeline(out_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    // Viewport, scissor & depth bias
    VkViewport vp{};
    vp.x = static_cast<float>(g_ctx.vp_x);
    vp.y = static_cast<float>(g_ctx.vp_y);
    vp.width = static_cast<float>(std::max(1, g_ctx.vp_w));
    vp.height = static_cast<float>(std::max(1, g_ctx.vp_h));
    vp.minDepth = g_ctx.depth_near;
    vp.maxDepth = g_ctx.depth_far;
    g_ctx.vk.CmdSetViewport(out_cmd, 0, 1, &vp);

    VkRect2D sci{};
    sci.offset = {0, 0};
    sci.extent = {g_ctx.current_fb_w, g_ctx.current_fb_h};
    g_ctx.vk.CmdSetScissor(out_cmd, 0, 1, &sci);

    if (g_ctx.poly_offset_enable) {
        g_ctx.vk.CmdSetDepthBias(out_cmd, g_ctx.poly_units, g_ctx.poly_clamp, g_ctx.poly_factor);
    } else {
        g_ctx.vk.CmdSetDepthBias(out_cmd, 0.0f, 0.0f, 0.0f);
    }

    // Bind vertex buffer if attributes active
    if (vao && !vk_attribs.empty()) {
        GLuint vbo_id = 0;
        for (const auto& a : vao->attribs) {
            if (a.enabled && a.vbo) { vbo_id = a.vbo; break; }
        }
        if (vbo_id && g_ctx.buffers.count(vbo_id)) {
            const VkBufObj& vb = g_ctx.buffers[vbo_id];
            const VkBuffer b = vb.active_buf ? vb.active_buf : g_ctx.dummy_ubo_buf;
            const VkDeviceSize off = vb.active_buf ? vb.active_offset : 0;
            g_ctx.vk.CmdBindVertexBuffers(out_cmd, 0, 1, &b, &off);
        }
    }

    // Allocate & populate per-draw descriptor set
    if (!prog.ubos.empty() || !prog.samplers.empty()) {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = g_ctx.desc_pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &prog.set_layout;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        if (g_ctx.vk.AllocateDescriptorSets(g_ctx.device, &dsai, &ds) == VK_SUCCESS) {
            std::vector<VkDescriptorBufferInfo> buf_infos(prog.ubos.size());
            std::vector<VkDescriptorImageInfo> img_infos(prog.samplers.size());
            std::vector<VkWriteDescriptorSet> writes;
            writes.reserve(prog.ubos.size() + prog.samplers.size());

            for (size_t i = 0; i < prog.ubos.size(); ++i) {
                const ReflectedUbo& u = prog.ubos[i];
                VkDescriptorBufferInfo& bi = buf_infos[i];
                bi.buffer = g_ctx.dummy_ubo_buf;
                bi.offset = 0;
                bi.range = VK_WHOLE_SIZE;
                if (u.gl_slot < 64 && g_ctx.ubo_slots[u.gl_slot].buf != 0) {
                    const VkContext::UboSlot& sl = g_ctx.ubo_slots[u.gl_slot];
                    auto bit = g_ctx.buffers.find(sl.buf);
                    if (bit != g_ctx.buffers.end() && bit->second.active_buf) {
                        bi.buffer = bit->second.active_buf;
                        bi.offset = bit->second.active_offset + sl.offset;
                        bi.range = (sl.size > 0) ? sl.size : std::max<VkDeviceSize>(16, bit->second.active_size - sl.offset);
                    }
                }
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = ds;
                w.dstBinding = u.binding;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.pBufferInfo = &bi;
                writes.push_back(w);
            }

            for (size_t i = 0; i < prog.samplers.size(); ++i) {
                const ReflectedSampler& s = prog.samplers[i];
                VkDescriptorImageInfo& ii = img_infos[i];
                ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ii.imageView = s.is_cube ? g_ctx.dummy_cube.sample_view
                                         : (s.is_array ? g_ctx.dummy_depth_array.sample_view
                                                       : (s.is_shadow ? g_ctx.dummy_depth_2d.sample_view : g_ctx.dummy_white_2d.sample_view));
                ii.sampler = s.is_shadow ? g_ctx.dummy_shadow_sampler : g_ctx.dummy_linear_sampler;

                const GLuint unit = static_cast<GLuint>(std::clamp(s.bound_unit, 0, 63));
                const GLuint tid = g_ctx.tex_units[unit];
                if (tid != 0 && tid != fbo.color_tex && tid != fbo.depth_tex) {
                    auto tit = g_ctx.textures.find(tid);
                    if (tit != g_ctx.textures.end() && tit->second.sample_view) {
                        const bool tex_is_cube = (tit->second.target == GL_TEXTURE_CUBE_MAP);
                        const bool tex_is_arr = (tit->second.target == GL_TEXTURE_2D_ARRAY);
                        if (tex_is_cube == s.is_cube && tex_is_arr == s.is_array) {
                            ii.imageView = tit->second.sample_view;
                        }
                    }
                }
                const GLuint sid = g_ctx.smp_units[unit];
                if (sid != 0 && g_ctx.samplers.count(sid)) {
                    VkSamplerObj& sobj = g_ctx.samplers[sid];
                    const bool smp_is_shadow = (sobj.compare_mode == GL_COMPARE_REF_TO_TEXTURE);
                    if (smp_is_shadow == s.is_shadow) {
                        ii.sampler = ensure_vk_sampler(sobj);
                    }
                }

                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = ds;
                w.dstBinding = s.binding;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &ii;
                writes.push_back(w);
            }

            g_ctx.vk.UpdateDescriptorSets(g_ctx.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            g_ctx.vk.CmdBindDescriptorSets(out_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prog.pipe_layout, 0, 1, &ds, 0, nullptr);
        }
    }
    return true;
}

bool create_swapchain_resources(int req_w, int req_h) {
    if (g_ctx.headless || !g_ctx.surface) return true;
    g_ctx.vk.DeviceWaitIdle(g_ctx.device);

    if (g_ctx.swapchain) {
        g_ctx.vk.DestroySwapchainKHR(g_ctx.device, g_ctx.swapchain, nullptr);
        g_ctx.swapchain = VK_NULL_HANDLE;
    }

    VkSurfaceCapabilitiesKHR caps{};
    g_ctx.vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(g_ctx.phys_dev, g_ctx.surface, &caps);

    uint32_t fmt_count = 0;
    g_ctx.vk.GetPhysicalDeviceSurfaceFormatsKHR(g_ctx.phys_dev, g_ctx.surface, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
    g_ctx.vk.GetPhysicalDeviceSurfaceFormatsKHR(g_ctx.phys_dev, g_ctx.surface, &fmt_count, fmts.data());

    VkSurfaceFormatKHR chosen = fmts.empty()
                                    ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                    : fmts[0];
    for (const auto& f : fmts) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    g_ctx.swap_format = chosen.format;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = std::clamp<uint32_t>(static_cast<uint32_t>(std::max(1, req_w)), caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp<uint32_t>(static_cast<uint32_t>(std::max(1, req_h)), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    g_ctx.swap_extent = ext;

    uint32_t img_count = std::max<uint32_t>(2u, caps.minImageCount);
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount) img_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = g_ctx.surface;
    sci.minImageCount = img_count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                           : caps.currentTransform;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR cand : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                             VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR}) {
        if (caps.supportedCompositeAlpha & cand) { alpha = cand; break; }
    }
    sci.compositeAlpha = alpha;
    const char* vsync_env = std::getenv("ME_VSYNC");
    sci.presentMode = (vsync_env && vsync_env[0] == '0') ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;

    if (g_ctx.vk.CreateSwapchainKHR(g_ctx.device, &sci, nullptr, &g_ctx.swapchain) != VK_SUCCESS) {
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (g_ctx.vk.CreateSwapchainKHR(g_ctx.device, &sci, nullptr, &g_ctx.swapchain) != VK_SUCCESS) return false;
    }

    uint32_t actual = 0;
    g_ctx.vk.GetSwapchainImagesKHR(g_ctx.device, g_ctx.swapchain, &actual, nullptr);
    g_ctx.swap_images.resize(actual);
    g_ctx.vk.GetSwapchainImagesKHR(g_ctx.device, g_ctx.swapchain, &actual, g_ctx.swap_images.data());
    g_ctx.swap_layouts.assign(actual, VK_IMAGE_LAYOUT_UNDEFINED);
    for (VkSemaphore s : g_ctx.render_done_sems) {
        if (s) g_ctx.vk.DestroySemaphore(g_ctx.device, s, nullptr);
    }
    g_ctx.render_done_sems.resize(actual, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < actual; ++i) {
        g_ctx.vk.CreateSemaphore(g_ctx.device, &smci, nullptr, &g_ctx.render_done_sems[i]);
    }
    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
// Lifecycle Entry Points
// -----------------------------------------------------------------------------
void prepare_vulkan_library() {
#if defined(__APPLE__)
    if (!std::getenv("VK_ICD_FILENAMES")) {
        std::error_code ec;
        if (std::filesystem::exists("/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json", ec)) {
            setenv("VK_ICD_FILENAMES", "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json", 0);
        }
    }
    if (SDL_Vulkan_LoadLibrary("/opt/homebrew/lib/libMoltenVK.dylib") != 0) {
        SDL_Vulkan_LoadLibrary(nullptr);
    }
#else
    SDL_Vulkan_LoadLibrary(nullptr);
#endif
}

bool init_context(void* sdl_window, int width, int height, bool headless, const std::string& cache_dir) {
    if (g_ctx.initialized) return true;
    g_ctx.headless = headless;
    g_ctx.sdl_window = static_cast<SDL_Window*>(sdl_window);
    g_ctx.spirv_cache.load_from_disk(cache_dir);

    prepare_vulkan_library();
    if (!g_ctx.vk.load_library(!headless)) {
        std::cerr << "[VulkanRenderer] Could not load Vulkan library / vkGetInstanceProcAddr" << std::endl;
        return false;
    }

    uint32_t avail_ext_count = 0;
    g_ctx.vk.EnumerateInstanceExtensionProperties(nullptr, &avail_ext_count, nullptr);
    std::vector<VkExtensionProperties> avail_exts(avail_ext_count);
    g_ctx.vk.EnumerateInstanceExtensionProperties(nullptr, &avail_ext_count, avail_exts.data());
    auto has_inst_ext = [&](const char* name) {
        for (const auto& e : avail_exts) {
            if (std::strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    std::vector<const char*> inst_exts;
    if (!headless && g_ctx.sdl_window) {
        unsigned int count = 0;
        SDL_Vulkan_GetInstanceExtensions(g_ctx.sdl_window, &count, nullptr);
        inst_exts.resize(count);
        SDL_Vulkan_GetInstanceExtensions(g_ctx.sdl_window, &count, inst_exts.data());
    }
    VkInstanceCreateFlags inst_flags = 0;
    if (has_inst_ext(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        inst_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Mirror's Edge Native Vulkan";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "mierrorsedgere";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.flags = inst_flags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
    ici.ppEnabledExtensionNames = inst_exts.data();

    if (g_ctx.vk.CreateInstance(&ici, nullptr, &g_ctx.instance) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] vkCreateInstance failed" << std::endl;
        return false;
    }
    if (!g_ctx.vk.load_instance(g_ctx.instance)) return false;

    if (!headless && g_ctx.sdl_window) {
        if (!SDL_Vulkan_CreateSurface(g_ctx.sdl_window, g_ctx.instance, &g_ctx.surface)) {
            std::cerr << "[VulkanRenderer] SDL_Vulkan_CreateSurface failed: " << SDL_GetError() << std::endl;
            return false;
        }
    }

    uint32_t dev_count = 0;
    g_ctx.vk.EnumeratePhysicalDevices(g_ctx.instance, &dev_count, nullptr);
    if (dev_count == 0) {
        std::cerr << "[VulkanRenderer] No Vulkan physical devices found" << std::endl;
        return false;
    }
    std::vector<VkPhysicalDevice> devs(dev_count);
    g_ctx.vk.EnumeratePhysicalDevices(g_ctx.instance, &dev_count, devs.data());
    g_ctx.phys_dev = devs[0];
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p{};
        g_ctx.vk.GetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
            p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            g_ctx.phys_dev = d;
            break;
        }
    }

    g_ctx.vk.GetPhysicalDeviceProperties(g_ctx.phys_dev, &g_ctx.props);
    g_ctx.vk.GetPhysicalDeviceMemoryProperties(g_ctx.phys_dev, &g_ctx.mem_props);
    VkPhysicalDeviceFeatures sup{};
    g_ctx.vk.GetPhysicalDeviceFeatures(g_ctx.phys_dev, &sup);
    g_ctx.enabled_features.samplerAnisotropy = sup.samplerAnisotropy;
    g_ctx.enabled_features.textureCompressionBC = sup.textureCompressionBC;
    g_ctx.supports_bc = (sup.textureCompressionBC == VK_TRUE);

    g_ctx.renderer_str = g_ctx.props.deviceName;
    char ver_buf[64];
    std::snprintf(ver_buf, sizeof(ver_buf), "Vulkan %u.%u.%u",
                  VK_VERSION_MAJOR(g_ctx.props.apiVersion),
                  VK_VERSION_MINOR(g_ctx.props.apiVersion),
                  VK_VERSION_PATCH(g_ctx.props.apiVersion));
    g_ctx.version_str = ver_buf;
    g_ctx.vendor_str = "Khronos Vulkan";

    // Check R32_SFLOAT color attachment support
    VkFormatProperties fp_r32{};
    g_ctx.vk.GetPhysicalDeviceFormatProperties(g_ctx.phys_dev, VK_FORMAT_R32_SFLOAT, &fp_r32);
    if (!(fp_r32.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        g_ctx.r32f_format = VK_FORMAT_R16_SFLOAT;
    }

    uint32_t qf_count = 0;
    g_ctx.vk.GetPhysicalDeviceQueueFamilyProperties(g_ctx.phys_dev, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    g_ctx.vk.GetPhysicalDeviceQueueFamilyProperties(g_ctx.phys_dev, &qf_count, qfs.data());
    for (uint32_t i = 0; i < qf_count; ++i) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            if (g_ctx.surface) {
                VkBool32 present = VK_FALSE;
                g_ctx.vk.GetPhysicalDeviceSurfaceSupportKHR(g_ctx.phys_dev, i, g_ctx.surface, &present);
                if (!present) continue;
            }
            g_ctx.queue_family = i;
            break;
        }
    }

    uint32_t dext_count = 0;
    g_ctx.vk.EnumerateDeviceExtensionProperties(g_ctx.phys_dev, nullptr, &dext_count, nullptr);
    std::vector<VkExtensionProperties> dexts(dext_count);
    g_ctx.vk.EnumerateDeviceExtensionProperties(g_ctx.phys_dev, nullptr, &dext_count, dexts.data());
    auto has_dev_ext = [&](const char* n) {
        for (const auto& e : dexts) {
            if (std::strcmp(e.extensionName, n) == 0) return true;
        }
        return false;
    };

    std::vector<const char*> dev_exts;
    if (g_ctx.surface) dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (has_dev_ext(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
        dev_exts.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }

    const float qprio = 1.0f;
    VkDeviceQueueCreateInfo dqci{};
    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = g_ctx.queue_family;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &qprio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;
    dci.pEnabledFeatures = &g_ctx.enabled_features;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();

    if (g_ctx.vk.CreateDevice(g_ctx.phys_dev, &dci, nullptr, &g_ctx.device) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] vkCreateDevice failed" << std::endl;
        return false;
    }
    if (!g_ctx.vk.load_device(g_ctx.device)) return false;
    g_ctx.vk.GetDeviceQueue(g_ctx.device, g_ctx.queue_family, 0, &g_ctx.queue);

    // Command pool & command buffers
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g_ctx.queue_family;
    g_ctx.vk.CreateCommandPool(g_ctx.device, &cpci, nullptr, &g_ctx.cmd_pool);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_ctx.cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    g_ctx.vk.AllocateCommandBuffers(g_ctx.device, &cbai, &g_ctx.frame_cmd);
    g_ctx.vk.AllocateCommandBuffers(g_ctx.device, &cbai, &g_ctx.upload_cmd);

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    g_ctx.vk.CreateFence(g_ctx.device, &fci, nullptr, &g_ctx.frame_fence);
    g_ctx.vk.CreateFence(g_ctx.device, &fci, nullptr, &g_ctx.upload_fence);

    VkSemaphoreCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    g_ctx.vk.CreateSemaphore(g_ctx.device, &smci, nullptr, &g_ctx.acquire_sem);

    // Ring & staging buffers
    void* ring_map = nullptr;
    create_vk_buffer(VkContext::kRingSize,
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_ctx.ring_buf, g_ctx.ring_mem, &ring_map);
    g_ctx.ring_ptr = static_cast<uint8_t*>(ring_map);

    void* stg_map = nullptr;
    create_vk_buffer(VkContext::kStagingSize,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_ctx.staging_buf, g_ctx.staging_mem, &stg_map);
    g_ctx.staging_ptr = static_cast<uint8_t*>(stg_map);

    void* zero_map = nullptr;
    create_vk_buffer(16384, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_ctx.dummy_ubo_buf, g_ctx.dummy_ubo_mem, &zero_map);
    if (zero_map) {
        std::memset(zero_map, 0, 16384);
        g_ctx.vk.UnmapMemory(g_ctx.device, g_ctx.dummy_ubo_mem);
    }

    // Descriptor pool & pipeline cache
    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 131072},
    };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 32768;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = pool_sizes;
    g_ctx.vk.CreateDescriptorPool(g_ctx.device, &dpci, nullptr, &g_ctx.desc_pool);

    VkPipelineCacheCreateInfo pcci{};
    pcci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    g_ctx.vk.CreatePipelineCache(g_ctx.device, &pcci, nullptr, &g_ctx.pipe_cache);

    // Dummy textures & samplers
    g_ctx.dummy_white_2d.target = GL_TEXTURE_2D;
    allocate_vk_image(g_ctx.dummy_white_2d, 1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    const uint32_t white = 0xFFFFFFFFu;
    upload_mip_bytes(g_ctx.dummy_white_2d, 0, 0, 1, 1, &white, 4);

    g_ctx.dummy_cube.target = GL_TEXTURE_CUBE_MAP;
    allocate_vk_image(g_ctx.dummy_cube, 1, 1, 6, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    for (uint32_t f = 0; f < 6; ++f) {
        upload_mip_bytes(g_ctx.dummy_cube, 0, f, 1, 1, &white, 4);
    }

    g_ctx.dummy_depth_2d.target = GL_TEXTURE_2D;
    allocate_vk_image(g_ctx.dummy_depth_2d, 1, 1, 1, 1, g_ctx.depth_format, true, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    transition_all_subresources(get_upload_cmd(), g_ctx.dummy_depth_2d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    g_ctx.dummy_depth_array.target = GL_TEXTURE_2D_ARRAY;
    allocate_vk_image(g_ctx.dummy_depth_array, 1, 1, 3, 1, g_ctx.depth_format, true, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    transition_all_subresources(get_upload_cmd(), g_ctx.dummy_depth_array, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    flush_upload_cmd();

    VkSamplerCreateInfo lsci{};
    lsci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    lsci.magFilter = VK_FILTER_LINEAR;
    lsci.minFilter = VK_FILTER_LINEAR;
    lsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    lsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    g_ctx.vk.CreateSampler(g_ctx.device, &lsci, nullptr, &g_ctx.dummy_linear_sampler);
    lsci.compareEnable = VK_TRUE;
    lsci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    g_ctx.vk.CreateSampler(g_ctx.device, &lsci, nullptr, &g_ctx.dummy_shadow_sampler);

    if (!headless && !create_swapchain_resources(width, height)) return false;

    g_ctx.initialized = true;
    return true;
}

void shutdown_context() {
    if (!g_ctx.initialized) return;
    g_ctx.vk.DeviceWaitIdle(g_ctx.device);
    g_ctx.spirv_cache.save_to_disk();
    g_ctx.initialized = false;
}

bool loaded() { return g_ctx.initialized; }
void unload() { shutdown_context(); }

void resize_swapchain(int width, int height) {
    if (!g_ctx.initialized || g_ctx.headless) return;
    create_swapchain_resources(width, height);
}

void begin_frame() {
    if (!g_ctx.initialized) return;
    flush_upload_cmd();

    g_ctx.vk.WaitForFences(g_ctx.device, 1, &g_ctx.frame_fence, VK_TRUE, UINT64_MAX);
    g_ctx.vk.ResetFences(g_ctx.device, 1, &g_ctx.frame_fence);
    g_ctx.vk.ResetDescriptorPool(g_ctx.device, g_ctx.desc_pool, 0);
    for (auto& c : g_ctx.transient_staging) {
        if (c.ptr) g_ctx.vk.UnmapMemory(g_ctx.device, c.mem);
        if (c.buf) g_ctx.vk.DestroyBuffer(g_ctx.device, c.buf, nullptr);
        if (c.mem) g_ctx.vk.FreeMemory(g_ctx.device, c.mem, nullptr);
    }
    g_ctx.transient_staging.clear();
    g_ctx.ring_offset = 0;
    g_ctx.staging_offset = 0;

    g_ctx.has_acquired_image = false;
    if (!g_ctx.headless && g_ctx.swapchain) {
        VkResult r = g_ctx.vk.AcquireNextImageKHR(g_ctx.device, g_ctx.swapchain, UINT64_MAX,
                                                  g_ctx.acquire_sem, VK_NULL_HANDLE, &g_ctx.current_swap_image);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            int dw = 0, dh = 0;
            SDL_Vulkan_GetDrawableSize(g_ctx.sdl_window, &dw, &dh);
            create_swapchain_resources(dw, dh);
            r = g_ctx.vk.AcquireNextImageKHR(g_ctx.device, g_ctx.swapchain, UINT64_MAX,
                                             g_ctx.acquire_sem, VK_NULL_HANDLE, &g_ctx.current_swap_image);
        }
        g_ctx.has_acquired_image = (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR);
    }

    g_ctx.vk.ResetCommandBuffer(g_ctx.frame_cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_ctx.vk.BeginCommandBuffer(g_ctx.frame_cmd, &bi);
    g_ctx.in_frame = true;
}

void end_frame(GLuint present_tex, int src_w, int src_h) {
    if (!g_ctx.in_frame) return;
    end_active_render_pass();

    const GLuint tex_id = present_tex ? present_tex : g_ctx.last_present_candidate_tex;
    if (g_ctx.has_acquired_image && tex_id && g_ctx.textures.count(tex_id)) {
        VkTexObj& src = g_ctx.textures[tex_id];
        transition_subresource(g_ctx.frame_cmd, src, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkImage dst_img = g_ctx.swap_images[g_ctx.current_swap_image];
        VkImageMemoryBarrier b1{};
        b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b1.oldLayout = g_ctx.swap_layouts[g_ctx.current_swap_image];
        b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.image = dst_img;
        b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        g_ctx.vk.CmdPipelineBarrier(g_ctx.frame_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1);

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {src_w > 0 ? src_w : src.width, src_h > 0 ? src_h : src.height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(g_ctx.swap_extent.width), static_cast<int32_t>(g_ctx.swap_extent.height), 1};
        g_ctx.vk.CmdBlitImage(g_ctx.frame_cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              dst_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        transition_subresource(g_ctx.frame_cmd, src, 0, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        VkImageMemoryBarrier b2{};
        b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b2.image = dst_img;
        b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        g_ctx.vk.CmdPipelineBarrier(g_ctx.frame_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);
        g_ctx.swap_layouts[g_ctx.current_swap_image] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

    g_ctx.vk.EndCommandBuffer(g_ctx.frame_cmd);
    g_ctx.in_frame = false;

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_ctx.frame_cmd;
    VkSemaphore render_done_sem = VK_NULL_HANDLE;
    if (g_ctx.has_acquired_image && g_ctx.current_swap_image < g_ctx.render_done_sems.size()) {
        render_done_sem = g_ctx.render_done_sems[g_ctx.current_swap_image];
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &g_ctx.acquire_sem;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &render_done_sem;
    }
    g_ctx.vk.QueueSubmit(g_ctx.queue, 1, &si, g_ctx.frame_fence);

    if (g_ctx.has_acquired_image && render_done_sem) {
        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &render_done_sem;
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_ctx.swapchain;
        pi.pImageIndices = &g_ctx.current_swap_image;
        const VkResult pr = g_ctx.vk.QueuePresentKHR(g_ctx.queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            int dw = 0, dh = 0;
            SDL_Vulkan_GetDrawableSize(g_ctx.sdl_window, &dw, &dh);
            create_swapchain_resources(dw, dh);
        }
    }
    g_ctx.spirv_cache.save_to_disk();
}

void prewarm_spirv_parallel(const std::vector<std::pair<std::string, bool>>& sources) {
    const size_t n = sources.size();
    if (n == 0) return;
    std::atomic<size_t> next{0};
    const unsigned hw = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    workers.reserve(hw);
    for (unsigned t = 0; t < hw; ++t) {
        workers.emplace_back([&]() {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= n) break;
                const uint64_t h = fnv1a_hash(sources[i].first, sources[i].second);
                std::vector<uint32_t> spv;
                if (g_ctx.spirv_cache.lookup(h, spv)) continue;
                std::string log;
                if (compile_glsl_to_spirv_uncached(sources[i].first, sources[i].second, spv, log)) {
                    g_ctx.spirv_cache.insert(h, spv);
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    g_ctx.spirv_cache.save_to_disk();
}

void* native_vk_device() { return reinterpret_cast<void*>(g_ctx.device); }
std::string device_name_string() { return g_ctx.renderer_str; }

bool has_extension(const char* name) {
    if (!name) return false;
    if (std::strcmp(name, "GL_EXT_texture_compression_s3tc") == 0 ||
        std::strcmp(name, "GL_EXT_texture_compression_s3tc_srgb") == 0) {
        return g_ctx.supports_bc;
    }
    if (std::strcmp(name, "GL_EXT_texture_filter_anisotropic") == 0 ||
        std::strcmp(name, "GL_ARB_texture_filter_anisotropic") == 0) {
        return g_ctx.enabled_features.samplerAnisotropy == VK_TRUE;
    }
    if (std::strcmp(name, "GL_EXT_color_buffer_float") == 0 ||
        std::strcmp(name, "GL_EXT_color_buffer_half_float") == 0 ||
        std::strcmp(name, "GL_EXT_texture_norm16") == 0) {
        return true;
    }
    return false;
}

bool is_es() { return false; }
int version_major() { return 4; }
int version_minor() { return 5; }

// -----------------------------------------------------------------------------
// State & Resource API Implementations
// -----------------------------------------------------------------------------
const GLubyte* glGetString(GLenum name) {
    if (name == GL_RENDERER) return reinterpret_cast<const GLubyte*>(g_ctx.renderer_str.c_str());
    if (name == GL_VERSION) return reinterpret_cast<const GLubyte*>(g_ctx.version_str.c_str());
    if (name == GL_VENDOR) return reinterpret_cast<const GLubyte*>(g_ctx.vendor_str.c_str());
    return reinterpret_cast<const GLubyte*>("4.50 SPIR-V");
}

void glGetIntegerv(GLenum pname, GLint* data) {
    if (!data) return;
    switch (pname) {
        case GL_MAX_TEXTURE_IMAGE_UNITS: *data = 48; break;
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *data = 48; break;
        case GL_MAX_VARYING_VECTORS: *data = 32; break;
        case GL_MAX_TEXTURE_SIZE: *data = static_cast<GLint>(std::min<uint32_t>(16384u, g_ctx.props.limits.maxImageDimension2D)); break;
        case GL_MAX_3D_TEXTURE_SIZE: *data = static_cast<GLint>(std::min<uint32_t>(4096u, g_ctx.props.limits.maxImageDimension2D)); break;
        case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT:
            *data = static_cast<GLint>(std::max<VkDeviceSize>(256, g_ctx.props.limits.minUniformBufferOffsetAlignment));
            break;
        case GL_MAX_UNIFORM_BUFFER_BINDINGS: *data = 48; break;
        case GL_MAX_VERTEX_UNIFORM_BLOCKS: *data = 16; break;
        case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: *data = 16; break;
        case GL_MAX_UNIFORM_BLOCK_SIZE: *data = 65536; break;
        default: *data = 0; break;
    }
}

GLenum glGetError() { return GL_NO_ERROR; }

void glEnable(GLenum cap) {
    if (cap == GL_BLEND) g_ctx.blend_enable = true;
    else if (cap == GL_CULL_FACE) g_ctx.cull_enable = true;
    else if (cap == GL_DEPTH_TEST) g_ctx.depth_test = true;
    else if (cap == GL_POLYGON_OFFSET_FILL) g_ctx.poly_offset_enable = true;
}

void glDisable(GLenum cap) {
    if (cap == GL_BLEND) g_ctx.blend_enable = false;
    else if (cap == GL_CULL_FACE) g_ctx.cull_enable = false;
    else if (cap == GL_DEPTH_TEST) g_ctx.depth_test = false;
    else if (cap == GL_POLYGON_OFFSET_FILL) g_ctx.poly_offset_enable = false;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    g_ctx.vp_x = x;
    g_ctx.vp_y = y;
    g_ctx.vp_w = width;
    g_ctx.vp_h = height;
}

void glDepthRange(GLdouble n, GLdouble f) {
    g_ctx.depth_near = static_cast<float>(n);
    g_ctx.depth_far = static_cast<float>(f);
}

void glDepthRangef(GLfloat n, GLfloat f) {
    g_ctx.depth_near = n;
    g_ctx.depth_far = f;
}

void glDepthFunc(GLenum func) { g_ctx.depth_func = func; }
void glDepthMask(GLboolean flag) { g_ctx.depth_write = (flag != GL_FALSE); }

void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    g_ctx.clear_rgba[0] = r;
    g_ctx.clear_rgba[1] = g;
    g_ctx.clear_rgba[2] = b;
    g_ctx.clear_rgba[3] = a;
}

void glClearDepth(GLdouble d) { g_ctx.clear_depth_val = static_cast<float>(d); }
void glClearDepthf(GLfloat d) { g_ctx.clear_depth_val = d; }

void glClear(GLbitfield mask) {
    const bool cc = (mask & GL_COLOR_BUFFER_BIT) != 0;
    const bool cd = (mask & GL_DEPTH_BUFFER_BIT) != 0;
    if (cc || cd) ensure_render_pass_open(cc, g_ctx.clear_rgba, cd, g_ctx.clear_depth_val);
}

void glClearBufferfv(GLenum buffer, GLint /*drawbuffer*/, const GLfloat* value) {
    if (buffer == GL_COLOR) ensure_render_pass_open(true, value, false, g_ctx.clear_depth_val);
    else if (buffer == GL_DEPTH) ensure_render_pass_open(false, nullptr, true, value ? value[0] : 1.0f);
}

void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
    g_ctx.blend_src_rgb = sfactorRGB;
    g_ctx.blend_dst_rgb = dfactorRGB;
    g_ctx.blend_src_a = sfactorAlpha;
    g_ctx.blend_dst_a = dfactorAlpha;
}

void glBlendEquation(GLenum mode) { g_ctx.blend_eq = mode; }
void glCullFace(GLenum mode) { g_ctx.cull_face = mode; }
void glFrontFace(GLenum mode) { g_ctx.front_face = mode; }

void glPolygonOffset(GLfloat factor, GLfloat units) {
    g_ctx.poly_factor = factor;
    g_ctx.poly_units = units;
    g_ctx.poly_clamp = 0.0f;
}

void glPolygonOffsetClamp(GLfloat factor, GLfloat units, GLfloat clamp) {
    g_ctx.poly_factor = factor;
    g_ctx.poly_units = units;
    g_ctx.poly_clamp = clamp;
}

void glPixelStorei(GLenum, GLint) {}
void glFlush() {}
void glFinish() {
    if (g_ctx.initialized) {
        flush_upload_cmd();
        g_ctx.vk.WaitForFences(g_ctx.device, 1, &g_ctx.frame_fence, VK_TRUE, UINT64_MAX);
    }
}

void glReadPixels(GLint /*x*/, GLint /*y*/, GLsizei width, GLsizei height, GLenum /*format*/, GLenum /*type*/, void* pixels) {
    if (!pixels || width <= 0 || height <= 0) return;
    GLuint src_tex = g_ctx.last_present_candidate_tex;
    if (g_ctx.read_fbo && g_ctx.fbos.count(g_ctx.read_fbo)) {
        const GLuint ct = g_ctx.fbos[g_ctx.read_fbo].color_tex;
        if (ct) src_tex = ct;
    }
    if (!src_tex || !g_ctx.textures.count(src_tex)) return;

    end_active_render_pass();
    flush_upload_cmd();
    g_ctx.vk.WaitForFences(g_ctx.device, 1, &g_ctx.frame_fence, VK_TRUE, UINT64_MAX);

    VkTexObj& tex = g_ctx.textures[src_tex];
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    VkBuffer rb_buf = VK_NULL_HANDLE;
    VkDeviceMemory rb_mem = VK_NULL_HANDLE;
    void* rb_map = nullptr;
    if (!create_vk_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          rb_buf, rb_mem, &rb_map)) {
        return;
    }

    VkCommandBuffer cmd = get_upload_cmd();
    transition_subresource(cmd, tex, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(std::min<int>(width, tex.width)),
                          static_cast<uint32_t>(std::min<int>(height, tex.height)), 1};
    g_ctx.vk.CmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &region);
    transition_subresource(cmd, tex, 0, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    flush_upload_cmd();

    std::memcpy(pixels, rb_map, bytes);
    g_ctx.vk.UnmapMemory(g_ctx.device, rb_mem);
    g_ctx.vk.DestroyBuffer(g_ctx.device, rb_buf, nullptr);
    g_ctx.vk.FreeMemory(g_ctx.device, rb_mem, nullptr);
}

void glReadBuffer(GLenum) {}
void glDrawBuffer(GLenum) {}
void glDrawBuffers(GLsizei, const GLenum*) {}
void glActiveTexture(GLenum texture) { g_ctx.active_tex_unit = std::min<GLuint>(63u, texture - GL_TEXTURE0); }

void glGenTextures(GLsizei n, GLuint* textures) {
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint id = g_ctx.next_id++;
        g_ctx.textures[id] = VkTexObj{};
        textures[i] = id;
    }
}

void glDeleteTextures(GLsizei n, const GLuint* textures) {
    if (!g_ctx.initialized || n <= 0) return;
    bool waited = false;
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint id = textures[i];
        auto it = g_ctx.textures.find(id);
        if (it == g_ctx.textures.end()) continue;
        if (!waited) {
            flush_upload_cmd();
            g_ctx.vk.DeviceWaitIdle(g_ctx.device);
            waited = true;
        }
        if (it->second.sample_view) g_ctx.vk.DestroyImageView(g_ctx.device, it->second.sample_view, nullptr);
        for (VkImageView v : it->second.layer_views) {
            if (v) g_ctx.vk.DestroyImageView(g_ctx.device, v, nullptr);
        }
        if (it->second.image) g_ctx.vk.DestroyImage(g_ctx.device, it->second.image, nullptr);
        if (it->second.memory) g_ctx.vk.FreeMemory(g_ctx.device, it->second.memory, nullptr);
        g_ctx.textures.erase(it);
    }
}

void glBindTexture(GLenum target, GLuint texture) {
    g_ctx.tex_units[g_ctx.active_tex_unit] = texture;
    if (texture && g_ctx.textures.count(texture)) g_ctx.textures[texture].target = target;
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint /*border*/,
                  GLenum format, GLenum /*type*/, const void* pixels) {
    const GLuint id = g_ctx.tex_units[g_ctx.active_tex_unit];
    if (!id) return;
    VkTexObj& tex = g_ctx.textures[id];
    const uint32_t layer =
        (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6)
            ? static_cast<uint32_t>(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X)
            : 0u;
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6) {
        tex.target = GL_TEXTURE_CUBE_MAP;
    }

    VkFormat vk_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    bool is_depth = false;
    VkImageUsageFlags extra_usage = 0;
    size_t bpp = 4;

    switch (internalformat) {
        case GL_RGBA8:
        case GL_RGBA:
            vk_fmt = VK_FORMAT_R8G8B8A8_UNORM;
            extra_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            bpp = 4;
            break;
        case GL_SRGB8_ALPHA8:
        case GL_SRGB8:
            vk_fmt = VK_FORMAT_R8G8B8A8_SRGB;
            bpp = 4;
            break;
        case GL_R8:
            vk_fmt = VK_FORMAT_R8_UNORM;
            bpp = 1;
            break;
        case GL_RG8_SNORM:
            vk_fmt = VK_FORMAT_R8G8_SNORM;
            bpp = 2;
            break;
        case GL_RGBA16:
        case GL_RGBA16F:
            vk_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
            extra_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            bpp = 8;
            break;
        case GL_R16F:
            vk_fmt = VK_FORMAT_R16_SFLOAT;
            extra_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            bpp = 2;
            break;
        case GL_R32F:
            vk_fmt = g_ctx.r32f_format;
            extra_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            bpp = (vk_fmt == VK_FORMAT_R32_SFLOAT) ? 4 : 2;
            break;
        case GL_DEPTH_COMPONENT32F:
        case GL_DEPTH_COMPONENT24:
            vk_fmt = g_ctx.depth_format;
            is_depth = true;
            extra_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            bpp = 4;
            break;
        default:
            vk_fmt = VK_FORMAT_R8G8B8A8_UNORM;
            extra_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            bpp = 4;
            break;
    }

    if (level == 0 && layer == 0) {
        const uint32_t full_mips = pixels
                                       ? (1u + static_cast<uint32_t>(std::floor(std::log2(std::max({1, width, height})))))
                                       : 1u;
        const int num_layers = (tex.target == GL_TEXTURE_CUBE_MAP) ? 6 : 1;
        allocate_vk_image(tex, width, height, num_layers, full_mips, vk_fmt, is_depth, extra_usage);
    }
    if (!pixels) return;

    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    const auto* src = static_cast<const uint8_t*>(pixels);
    if (format == GL_BGRA || format == GL_BGRA_EXT) {
        std::vector<uint8_t> rgba(n * 4);
        for (size_t i = 0; i < n; ++i) {
            rgba[i * 4 + 0] = src[i * 4 + 2];
            rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4 + 0];
            rgba[i * 4 + 3] = src[i * 4 + 3];
        }
        upload_mip_bytes(tex, static_cast<uint32_t>(level), layer, width, height, rgba.data(), rgba.size());
    } else if (vk_fmt == VK_FORMAT_R8G8B8A8_SRGB && format == GL_RED) {
        std::vector<uint8_t> rgba(n * 4, 255);
        for (size_t i = 0; i < n; ++i) rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = src[i];
        upload_mip_bytes(tex, static_cast<uint32_t>(level), layer, width, height, rgba.data(), rgba.size());
    } else if (vk_fmt == VK_FORMAT_R8G8B8A8_SRGB && format == GL_RGB) {
        std::vector<uint8_t> rgba(n * 4, 255);
        for (size_t i = 0; i < n; ++i) {
            rgba[i * 4 + 0] = src[i * 3 + 0];
            rgba[i * 4 + 1] = src[i * 3 + 1];
            rgba[i * 4 + 2] = src[i * 3 + 2];
        }
        upload_mip_bytes(tex, static_cast<uint32_t>(level), layer, width, height, rgba.data(), rgba.size());
    } else {
        upload_mip_bytes(tex, static_cast<uint32_t>(level), layer, width, height, src, n * bpp);
    }
}

void glTexImage3D(GLenum target, GLint /*level*/, GLint /*internalformat*/, GLsizei width, GLsizei height, GLsizei depth,
                  GLint /*border*/, GLenum /*format*/, GLenum /*type*/, const void* /*pixels*/) {
    const GLuint id = g_ctx.tex_units[g_ctx.active_tex_unit];
    if (!id) return;
    VkTexObj& tex = g_ctx.textures[id];
    tex.target = target;
    allocate_vk_image(tex, width, height, depth, 1, g_ctx.depth_format, true, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

void glTexSubImage2D(GLenum /*target*/, GLint level, GLint /*xoffset*/, GLint /*yoffset*/, GLsizei width, GLsizei height,
                     GLenum format, GLenum /*type*/, const void* pixels) {
    const GLuint id = g_ctx.tex_units[g_ctx.active_tex_unit];
    if (!id || !pixels) return;
    VkTexObj& tex = g_ctx.textures[id];
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    const auto* src = static_cast<const uint8_t*>(pixels);
    if (format == GL_BGRA || format == GL_BGRA_EXT) {
        std::vector<uint8_t> rgba(n * 4);
        for (size_t i = 0; i < n; ++i) {
            rgba[i * 4 + 0] = src[i * 4 + 2];
            rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4 + 0];
            rgba[i * 4 + 3] = src[i * 4 + 3];
        }
        upload_mip_bytes(tex, static_cast<uint32_t>(level), 0, width, height, rgba.data(), rgba.size());
    } else {
        const size_t bpp = (format == GL_RED) ? (tex.format == VK_FORMAT_R32_SFLOAT ? 4 : 2) : 4;
        upload_mip_bytes(tex, static_cast<uint32_t>(level), 0, width, height, src, n * bpp);
    }
}

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                            GLint /*border*/, GLsizei imageSize, const void* data) {
    const GLuint id = g_ctx.tex_units[g_ctx.active_tex_unit];
    if (!id || !data) return;
    VkTexObj& tex = g_ctx.textures[id];
    const uint32_t layer =
        (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6)
            ? static_cast<uint32_t>(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X)
            : 0u;
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6) {
        tex.target = GL_TEXTURE_CUBE_MAP;
    }

    me::BcFormat bc = me::BcFormat::BC1;
    VkFormat vk_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    switch (internalformat) {
        case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            bc = me::BcFormat::BC1;
            vk_fmt = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
            bc = me::BcFormat::BC1;
            vk_fmt = VK_FORMAT_R8G8B8A8_SRGB;
            break;
        case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
            bc = me::BcFormat::BC2;
            vk_fmt = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
            bc = me::BcFormat::BC2;
            vk_fmt = VK_FORMAT_R8G8B8A8_SRGB;
            break;
        case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
            bc = me::BcFormat::BC3;
            vk_fmt = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
            bc = me::BcFormat::BC3;
            vk_fmt = VK_FORMAT_R8G8B8A8_SRGB;
            break;
        default:
            break;
    }

    if (level == 0 && layer == 0) {
        const uint32_t full_mips = 1u + static_cast<uint32_t>(std::floor(std::log2(std::max({1, width, height}))));
        const int num_layers = (tex.target == GL_TEXTURE_CUBE_MAP) ? 6 : 1;
        allocate_vk_image(tex, width, height, num_layers, full_mips, vk_fmt, false, 0);
    }
    thread_local std::vector<uint8_t> rgba;
    if (me::bc_decode_rgba8(bc, width, height, static_cast<const uint8_t*>(data), static_cast<size_t>(imageSize), rgba)) {
        upload_mip_bytes(tex, static_cast<uint32_t>(level), layer, width, height, rgba.data(), rgba.size());
    }
}

void glTexParameteri(GLenum /*target*/, GLenum pname, GLint param) {
    const GLuint id = g_ctx.tex_units[g_ctx.active_tex_unit];
    if (!id || !g_ctx.textures.count(id)) return;
    VkTexObj& tex = g_ctx.textures[id];
    if (pname == GL_TEXTURE_MAX_LEVEL) {
        tex.max_level_param = param;
        if (tex.image) update_tex_sample_view(tex);
    } else if (pname == GL_TEXTURE_SWIZZLE_R || pname == GL_TEXTURE_SWIZZLE_G ||
               pname == GL_TEXTURE_SWIZZLE_B || pname == GL_TEXTURE_SWIZZLE_A) {
        auto map_swz = [](GLint s) -> VkComponentSwizzle {
            switch (s) {
                case GL_RED: return VK_COMPONENT_SWIZZLE_R;
                case GL_GREEN: return VK_COMPONENT_SWIZZLE_G;
                case GL_BLUE: return VK_COMPONENT_SWIZZLE_B;
                case GL_ALPHA: return VK_COMPONENT_SWIZZLE_A;
                case GL_ONE: return VK_COMPONENT_SWIZZLE_ONE;
                case GL_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
                default: return VK_COMPONENT_SWIZZLE_IDENTITY;
            }
        };
        const VkComponentSwizzle sv = map_swz(param);
        if (pname == GL_TEXTURE_SWIZZLE_R) tex.swizzle.r = sv;
        else if (pname == GL_TEXTURE_SWIZZLE_G) tex.swizzle.g = sv;
        else if (pname == GL_TEXTURE_SWIZZLE_B) tex.swizzle.b = sv;
        else if (pname == GL_TEXTURE_SWIZZLE_A) tex.swizzle.a = sv;
        if (tex.image) update_tex_sample_view(tex);
    }
}

void glTexParameteriv(GLenum /*target*/, GLenum pname, const GLint* params) {
    const GLuint id = g_ctx.tex_units[g_ctx.active_tex_unit];
    if (!id || !params || !g_ctx.textures.count(id)) return;
    VkTexObj& tex = g_ctx.textures[id];
    if (pname == GL_TEXTURE_SWIZZLE_RGBA) {
        auto map_swz = [](GLint s) -> VkComponentSwizzle {
            switch (s) {
                case GL_RED: return VK_COMPONENT_SWIZZLE_R;
                case GL_GREEN: return VK_COMPONENT_SWIZZLE_G;
                case GL_BLUE: return VK_COMPONENT_SWIZZLE_B;
                case GL_ALPHA: return VK_COMPONENT_SWIZZLE_A;
                case GL_ONE: return VK_COMPONENT_SWIZZLE_ONE;
                case GL_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
                default: return VK_COMPONENT_SWIZZLE_IDENTITY;
            }
        };
        tex.swizzle = {map_swz(params[0]), map_swz(params[1]), map_swz(params[2]), map_swz(params[3])};
        if (tex.image) update_tex_sample_view(tex);
    }
}

void glGenerateMipmap(GLenum /*target*/) {
    const GLuint id = g_ctx.tex_units[g_ctx.active_tex_unit];
    if (!id || !g_ctx.textures.count(id)) return;
    VkTexObj& tex = g_ctx.textures[id];
    if (tex.mip_levels <= 1) return;

    end_active_render_pass();
    VkCommandBuffer cmd = get_upload_cmd();
    int mw = tex.width;
    int mh = tex.height;
    for (uint32_t m = 1; m < tex.mip_levels; ++m) {
        transition_subresource(cmd, tex, m - 1, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition_subresource(cmd, tex, m, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        const int nw = std::max(1, mw / 2);
        const int nh = std::max(1, mh / 2);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        blit.srcOffsets[1] = {mw, mh, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        g_ctx.vk.CmdBlitImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        transition_subresource(cmd, tex, m - 1, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        mw = nw;
        mh = nh;
    }
    transition_subresource(cmd, tex, tex.mip_levels - 1, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    tex.valid_mips = tex.mip_levels;
    update_tex_sample_view(tex);
}

void glGenSamplers(GLsizei count, GLuint* samplers) {
    for (GLsizei i = 0; i < count; ++i) {
        const GLuint id = g_ctx.next_id++;
        g_ctx.samplers[id] = VkSamplerObj{};
        samplers[i] = id;
    }
}

void glDeleteSamplers(GLsizei count, const GLuint* samplers) {
    if (!g_ctx.initialized) return;
    for (GLsizei i = 0; i < count; ++i) {
        auto it = g_ctx.samplers.find(samplers[i]);
        if (it == g_ctx.samplers.end()) continue;
        if (it->second.sampler) g_ctx.vk.DestroySampler(g_ctx.device, it->second.sampler, nullptr);
        g_ctx.samplers.erase(it);
    }
}

void glBindSampler(GLuint unit, GLuint sampler) {
    if (unit < 64) g_ctx.smp_units[unit] = sampler;
}

void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param) {
    if (!g_ctx.samplers.count(sampler)) return;
    VkSamplerObj& s = g_ctx.samplers[sampler];
    if (pname == GL_TEXTURE_MIN_FILTER) s.min_filter = param;
    else if (pname == GL_TEXTURE_MAG_FILTER) s.mag_filter = param;
    else if (pname == GL_TEXTURE_WRAP_S) s.wrap_s = param;
    else if (pname == GL_TEXTURE_WRAP_T) s.wrap_t = param;
    else if (pname == GL_TEXTURE_WRAP_R) s.wrap_r = param;
    else if (pname == GL_TEXTURE_COMPARE_MODE) s.compare_mode = param;
    else if (pname == GL_TEXTURE_COMPARE_FUNC) s.compare_func = param;
    s.dirty = true;
}

void glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) {
    if (!g_ctx.samplers.count(sampler)) return;
    VkSamplerObj& s = g_ctx.samplers[sampler];
    if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT || pname == GL_TEXTURE_MAX_ANISOTROPY) {
        s.max_aniso = param;
        s.dirty = true;
    } else if (pname == GL_TEXTURE_MIN_LOD) {
        s.min_lod = param;
        s.dirty = true;
    } else if (pname == GL_TEXTURE_MAX_LOD) {
        s.max_lod = param;
        s.dirty = true;
    }
}

void glGenBuffers(GLsizei n, GLuint* buffers) {
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint id = g_ctx.next_id++;
        g_ctx.buffers[id] = VkBufObj{};
        buffers[i] = id;
    }
}

void glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    if (!g_ctx.initialized || n <= 0) return;
    bool waited = false;
    for (GLsizei i = 0; i < n; ++i) {
        auto it = g_ctx.buffers.find(buffers[i]);
        if (it == g_ctx.buffers.end()) continue;
        if (it->second.static_buf && !waited) {
            flush_upload_cmd();
            g_ctx.vk.DeviceWaitIdle(g_ctx.device);
            waited = true;
        }
        if (it->second.static_mapped) g_ctx.vk.UnmapMemory(g_ctx.device, it->second.static_mem);
        if (it->second.static_buf) g_ctx.vk.DestroyBuffer(g_ctx.device, it->second.static_buf, nullptr);
        if (it->second.static_mem) g_ctx.vk.FreeMemory(g_ctx.device, it->second.static_mem, nullptr);
        g_ctx.buffers.erase(it);
    }
}

void glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) g_ctx.bound_array_buf = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) g_ctx.bound_element_buf = buffer;
    else if (target == GL_UNIFORM_BUFFER) g_ctx.bound_uniform_buf = buffer;
}

GLuint target_buffer(GLenum target) {
    if (target == GL_ARRAY_BUFFER) return g_ctx.bound_array_buf;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return g_ctx.bound_element_buf;
    return g_ctx.bound_uniform_buf;
}

void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    const GLuint id = target_buffer(target);
    if (!id || size <= 0) return;
    VkBufObj& b = g_ctx.buffers[id];
    b.last_usage = usage;
    b.size = static_cast<size_t>(size);

    if (usage == GL_STATIC_DRAW || b.size > (VkContext::kRingSize / 4)) {
        if (b.static_buf) {
            flush_upload_cmd();
            g_ctx.vk.DeviceWaitIdle(g_ctx.device);
        }
        if (b.static_mapped) g_ctx.vk.UnmapMemory(g_ctx.device, b.static_mem);
        if (b.static_buf) g_ctx.vk.DestroyBuffer(g_ctx.device, b.static_buf, nullptr);
        if (b.static_mem) g_ctx.vk.FreeMemory(g_ctx.device, b.static_mem, nullptr);
        b.static_mapped = nullptr;
        create_vk_buffer(static_cast<VkDeviceSize>(size),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         b.static_buf, b.static_mem, &b.static_mapped);
        b.active_buf = b.static_buf;
        b.active_offset = 0;
        b.active_size = b.size;
        b.active_cpu = static_cast<uint8_t*>(b.static_mapped);
        if (data && b.active_cpu) std::memcpy(b.active_cpu, data, b.size);
        return;
    }

    // Sub-allocate lock-free slice from the 64 MB ring buffer
    const size_t align = std::max<size_t>(256, g_ctx.props.limits.minUniformBufferOffsetAlignment);
    size_t aligned = (g_ctx.ring_offset + align - 1) & ~(align - 1);
    if (aligned + b.size > VkContext::kRingSize) aligned = 0;
    b.active_buf = g_ctx.ring_buf;
    b.active_offset = aligned;
    b.active_size = b.size;
    b.active_cpu = g_ctx.ring_ptr + aligned;
    g_ctx.ring_offset = aligned + b.size;
    if (data) std::memcpy(b.active_cpu, data, b.size);
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    const GLuint id = target_buffer(target);
    if (!id || size <= 0 || !data) return;
    VkBufObj& b = g_ctx.buffers[id];
    const size_t total = std::max(b.size, static_cast<size_t>(offset + size));
    const uint8_t* prev = b.active_cpu;
    const size_t prev_sz = b.active_size;

    const size_t align = std::max<size_t>(256, g_ctx.props.limits.minUniformBufferOffsetAlignment);
    size_t aligned = (g_ctx.ring_offset + align - 1) & ~(align - 1);
    if (aligned + total > VkContext::kRingSize) aligned = 0;
    uint8_t* dst = g_ctx.ring_ptr + aligned;
    if (prev && prev != dst && (offset > 0 || static_cast<size_t>(offset + size) < prev_sz)) {
        std::memcpy(dst, prev, std::min(prev_sz, total));
    }
    std::memcpy(dst + offset, data, static_cast<size_t>(size));
    b.active_buf = g_ctx.ring_buf;
    b.active_offset = aligned;
    b.active_size = total;
    b.active_cpu = dst;
    g_ctx.ring_offset = aligned + total;
}

void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield /*access*/) {
    const GLuint id = target_buffer(target);
    if (!id || length <= 0) return nullptr;
    VkBufObj& b = g_ctx.buffers[id];
    if (!b.active_cpu || b.active_size < static_cast<size_t>(offset + length)) {
        glBufferData(target, offset + length, nullptr, GL_STREAM_DRAW);
    }
    return b.active_cpu + offset;
}

GLboolean glUnmapBuffer(GLenum) { return GL_TRUE; }

void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    if (target == GL_UNIFORM_BUFFER && index < 64) {
        g_ctx.bound_uniform_buf = buffer;
        g_ctx.ubo_slots[index] = {buffer, 0, 0};
    }
}

void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    if (target == GL_UNIFORM_BUFFER && index < 64) {
        g_ctx.bound_uniform_buf = buffer;
        g_ctx.ubo_slots[index] = {buffer, static_cast<VkDeviceSize>(offset), static_cast<VkDeviceSize>(size)};
    }
}

void glGenVertexArrays(GLsizei n, GLuint* arrays) {
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint id = g_ctx.next_id++;
        g_ctx.vaos[id] = VkVaoObj{};
        arrays[i] = id;
    }
}

void glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    for (GLsizei i = 0; i < n; ++i) g_ctx.vaos.erase(arrays[i]);
}

void glBindVertexArray(GLuint array) { g_ctx.bound_vao = array; }

void glEnableVertexAttribArray(GLuint index) {
    if (g_ctx.bound_vao && index < 16) g_ctx.vaos[g_ctx.bound_vao].attribs[index].enabled = true;
}

void glDisableVertexAttribArray(GLuint index) {
    if (g_ctx.bound_vao && index < 16) g_ctx.vaos[g_ctx.bound_vao].attribs[index].enabled = false;
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer) {
    if (g_ctx.bound_vao && index < 16) {
        g_ctx.vaos[g_ctx.bound_vao].attribs[index] = {
            true, size, type, normalized != GL_FALSE, false, stride,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer)), g_ctx.bound_array_buf
        };
    }
}

void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    if (g_ctx.bound_vao && index < 16) {
        g_ctx.vaos[g_ctx.bound_vao].attribs[index] = {
            true, size, type, false, true, stride,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer)), g_ctx.bound_array_buf
        };
    }
}

GLuint glCreateShader(GLenum type) {
    const GLuint id = g_ctx.next_id++;
    g_ctx.shaders[id] = VkShaderObj{type, {}, {}, {}, VK_NULL_HANDLE, false};
    return id;
}

void glDeleteShader(GLuint shader) {
    auto it = g_ctx.shaders.find(shader);
    if (it == g_ctx.shaders.end()) return;
    if (it->second.module && g_ctx.initialized) g_ctx.vk.DestroyShaderModule(g_ctx.device, it->second.module, nullptr);
    g_ctx.shaders.erase(it);
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    auto it = g_ctx.shaders.find(shader);
    if (it == g_ctx.shaders.end()) return;
    it->second.source.clear();
    for (GLsizei i = 0; i < count; ++i) {
        if (!string[i]) continue;
        if (length && length[i] >= 0) it->second.source.append(string[i], static_cast<size_t>(length[i]));
        else it->second.source.append(string[i]);
    }
}

void glCompileShader(GLuint shader) {
    auto it = g_ctx.shaders.find(shader);
    if (it == g_ctx.shaders.end()) return;
    VkShaderObj& s = it->second;
    const bool is_vert = (s.type == GL_VERTEX_SHADER);
    const uint64_t hash = fnv1a_hash(s.source, is_vert);

    std::vector<uint32_t> spv;
    if (!g_ctx.spirv_cache.lookup(hash, spv)) {
        if (!compile_glsl_to_spirv_uncached(s.source, is_vert, spv, s.log)) {
            s.compiled = false;
            return;
        }
        g_ctx.spirv_cache.insert(hash, spv);
    }
    s.spirv = spv;

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spv.size() * sizeof(uint32_t);
    smci.pCode = spv.data();
    if (g_ctx.vk.CreateShaderModule(g_ctx.device, &smci, nullptr, &s.module) == VK_SUCCESS) {
        s.compiled = true;
    } else {
        s.log = "vkCreateShaderModule failed";
        s.compiled = false;
    }
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    if (!params) return;
    const auto it = g_ctx.shaders.find(shader);
    if (it == g_ctx.shaders.end()) { *params = 0; return; }
    if (pname == GL_COMPILE_STATUS) *params = it->second.compiled ? GL_TRUE : GL_FALSE;
    else if (pname == GL_INFO_LOG_LENGTH) *params = static_cast<GLint>(it->second.log.size() + 1);
}

void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
    const auto it = g_ctx.shaders.find(shader);
    const std::string& log = (it != g_ctx.shaders.end()) ? it->second.log : "";
    const GLsizei n = std::min<GLsizei>(std::max<GLsizei>(0, bufSize - 1), static_cast<GLsizei>(log.size()));
    if (infoLog && n > 0) std::memcpy(infoLog, log.data(), static_cast<size_t>(n));
    if (infoLog && bufSize > 0) infoLog[n] = '\0';
    if (length) *length = n;
}

GLuint glCreateProgram() {
    const GLuint id = g_ctx.next_id++;
    g_ctx.programs[id] = VkProgramObj{};
    return id;
}

void glDeleteProgram(GLuint program) {
    auto it = g_ctx.programs.find(program);
    if (it == g_ctx.programs.end()) return;
    if (g_ctx.initialized) {
        if (it->second.vs_module) g_ctx.vk.DestroyShaderModule(g_ctx.device, it->second.vs_module, nullptr);
        if (it->second.fs_module) g_ctx.vk.DestroyShaderModule(g_ctx.device, it->second.fs_module, nullptr);
        if (it->second.pipe_layout) g_ctx.vk.DestroyPipelineLayout(g_ctx.device, it->second.pipe_layout, nullptr);
        if (it->second.set_layout) g_ctx.vk.DestroyDescriptorSetLayout(g_ctx.device, it->second.set_layout, nullptr);
    }
    g_ctx.programs.erase(it);
}

void glAttachShader(GLuint program, GLuint shader) {
    auto pit = g_ctx.programs.find(program);
    auto sit = g_ctx.shaders.find(shader);
    if (pit == g_ctx.programs.end() || sit == g_ctx.shaders.end()) return;
    if (sit->second.type == GL_VERTEX_SHADER) pit->second.vs_id = shader;
    else pit->second.fs_id = shader;
}

void glLinkProgram(GLuint program) {
    auto pit = g_ctx.programs.find(program);
    if (pit == g_ctx.programs.end()) return;
    VkProgramObj& prog = pit->second;
    prog.ubos.clear();
    prog.samplers.clear();

    if (prog.vs_id && g_ctx.shaders.count(prog.vs_id)) {
        const VkShaderObj& s = g_ctx.shaders[prog.vs_id];
        reflect_shader_source(s.source, prog.ubos, prog.samplers);
        if (!s.spirv.empty()) {
            VkShaderModuleCreateInfo smci{};
            smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            smci.codeSize = s.spirv.size() * sizeof(uint32_t);
            smci.pCode = s.spirv.data();
            g_ctx.vk.CreateShaderModule(g_ctx.device, &smci, nullptr, &prog.vs_module);
        }
    }
    if (prog.fs_id && g_ctx.shaders.count(prog.fs_id)) {
        const VkShaderObj& s = g_ctx.shaders[prog.fs_id];
        reflect_shader_source(s.source, prog.ubos, prog.samplers);
        if (!s.spirv.empty()) {
            VkShaderModuleCreateInfo smci{};
            smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            smci.codeSize = s.spirv.size() * sizeof(uint32_t);
            smci.pCode = s.spirv.data();
            g_ctx.vk.CreateShaderModule(g_ctx.device, &smci, nullptr, &prog.fs_module);
        }
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const auto& u : prog.ubos) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = u.binding;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    }
    for (const auto& s : prog.samplers) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = s.binding;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    }

    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = static_cast<uint32_t>(bindings.size());
    dslci.pBindings = bindings.data();
    g_ctx.vk.CreateDescriptorSetLayout(g_ctx.device, &dslci, nullptr, &prog.set_layout);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &prog.set_layout;
    g_ctx.vk.CreatePipelineLayout(g_ctx.device, &plci, nullptr, &prog.pipe_layout);

    prog.linked = true;
}

void glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    if (!params) return;
    const auto it = g_ctx.programs.find(program);
    if (it == g_ctx.programs.end()) { *params = 0; return; }
    if (pname == GL_LINK_STATUS) *params = it->second.linked ? GL_TRUE : GL_FALSE;
    else if (pname == GL_INFO_LOG_LENGTH) *params = static_cast<GLint>(it->second.log.size() + 1);
}

void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
    const auto it = g_ctx.programs.find(program);
    const std::string& log = (it != g_ctx.programs.end()) ? it->second.log : "";
    const GLsizei n = std::min<GLsizei>(std::max<GLsizei>(0, bufSize - 1), static_cast<GLsizei>(log.size()));
    if (infoLog && n > 0) std::memcpy(infoLog, log.data(), static_cast<size_t>(n));
    if (infoLog && bufSize > 0) infoLog[n] = '\0';
    if (length) *length = n;
}

void glUseProgram(GLuint program) { g_ctx.current_program = program; }

GLint glGetUniformLocation(GLuint program, const GLchar* name) {
    if (!name || !g_ctx.programs.count(program)) return -1;
    const auto& samps = g_ctx.programs[program].samplers;
    for (size_t i = 0; i < samps.size(); ++i) {
        if (samps[i].name == name) return static_cast<GLint>(i);
    }
    return -1;
}

void glUniform1i(GLint location, GLint v0) {
    if (location < 0 || !g_ctx.programs.count(g_ctx.current_program)) return;
    auto& samps = g_ctx.programs[g_ctx.current_program].samplers;
    if (static_cast<size_t>(location) < samps.size()) {
        samps[static_cast<size_t>(location)].bound_unit = v0;
    }
}

GLuint glGetUniformBlockIndex(GLuint program, const GLchar* uniformBlockName) {
    if (!uniformBlockName || !g_ctx.programs.count(program)) return GL_INVALID_INDEX;
    const auto& ubos = g_ctx.programs[program].ubos;
    for (size_t i = 0; i < ubos.size(); ++i) {
        if (ubos[i].block_name == uniformBlockName) return static_cast<GLuint>(i);
    }
    return GL_INVALID_INDEX;
}

void glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding) {
    if (!g_ctx.programs.count(program)) return;
    auto& ubos = g_ctx.programs[program].ubos;
    if (uniformBlockIndex < ubos.size()) ubos[uniformBlockIndex].gl_slot = uniformBlockBinding;
}

void glGenFramebuffers(GLsizei n, GLuint* framebuffers) {
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint id = g_ctx.next_id++;
        g_ctx.fbos[id] = VkFboObj{};
        framebuffers[i] = id;
    }
}

void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
    for (GLsizei i = 0; i < n; ++i) {
        if (g_ctx.draw_fbo == framebuffers[i]) {
            end_active_render_pass();
            g_ctx.draw_fbo = 0;
        }
        g_ctx.fbos.erase(framebuffers[i]);
    }
}

void glBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        if (g_ctx.rp_active && g_ctx.rp_fbo_id != framebuffer) end_active_render_pass();
        g_ctx.draw_fbo = framebuffer;
    }
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) {
        g_ctx.read_fbo = framebuffer;
    }
}

void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum /*textarget*/, GLuint texture, GLint level) {
    const GLuint fbo_id = (target == GL_READ_FRAMEBUFFER) ? g_ctx.read_fbo : g_ctx.draw_fbo;
    if (!fbo_id || !g_ctx.fbos.count(fbo_id)) return;
    VkFboObj& f = g_ctx.fbos[fbo_id];
    if (attachment == GL_COLOR_ATTACHMENT0) {
        f.color_tex = texture;
        f.color_level = level;
        f.color_layer = 0;
    } else if (attachment == GL_DEPTH_ATTACHMENT) {
        f.depth_tex = texture;
        f.depth_level = level;
        f.depth_layer = 0;
    }
}

void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    const GLuint fbo_id = (target == GL_READ_FRAMEBUFFER) ? g_ctx.read_fbo : g_ctx.draw_fbo;
    if (!fbo_id || !g_ctx.fbos.count(fbo_id)) return;
    VkFboObj& f = g_ctx.fbos[fbo_id];
    if (attachment == GL_DEPTH_ATTACHMENT) {
        f.depth_tex = texture;
        f.depth_level = level;
        f.depth_layer = layer;
    } else if (attachment == GL_COLOR_ATTACHMENT0) {
        f.color_tex = texture;
        f.color_level = level;
        f.color_layer = layer;
    }
}

GLenum glCheckFramebufferStatus(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }

void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                       GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                       GLbitfield mask, GLenum filter) {
    end_active_render_pass();
    if (!g_ctx.read_fbo || !g_ctx.fbos.count(g_ctx.read_fbo)) return;
    const VkFboObj& rf = g_ctx.fbos[g_ctx.read_fbo];

    if (g_ctx.draw_fbo == 0) {
        // Window presentation happens at end_frame(offscreen_color_tex); record candidate color target here.
        if (rf.color_tex) g_ctx.last_present_candidate_tex = rf.color_tex;
        return;
    }
    if (!g_ctx.fbos.count(g_ctx.draw_fbo)) return;
    const VkFboObj& df = g_ctx.fbos[g_ctx.draw_fbo];
    VkCommandBuffer cmd = get_upload_cmd();

    if ((mask & GL_COLOR_BUFFER_BIT) && rf.color_tex && df.color_tex &&
        g_ctx.textures.count(rf.color_tex) && g_ctx.textures.count(df.color_tex)) {
        VkTexObj& src = g_ctx.textures[rf.color_tex];
        VkTexObj& dst = g_ctx.textures[df.color_tex];
        transition_subresource(cmd, src, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition_subresource(cmd, dst, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (src.format == dst.format && src.width == dst.width && src.height == dst.height &&
            std::abs(srcX1 - srcX0) == src.width && std::abs(srcY1 - srcY0) == src.height &&
            std::abs(dstX1 - dstX0) == dst.width && std::abs(dstY1 - dstY0) == dst.height) {
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.extent = {static_cast<uint32_t>(src.width), static_cast<uint32_t>(src.height), 1};
            g_ctx.vk.CmdCopyImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        } else {
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {srcX0, srcY0, 0};
            blit.srcOffsets[1] = {srcX1, srcY1, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {dstX0, dstY0, 0};
            blit.dstOffsets[1] = {dstX1, dstY1, 1};
            g_ctx.vk.CmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                  filter == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        }
        transition_subresource(cmd, src, 0, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition_subresource(cmd, dst, 0, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    if ((mask & GL_DEPTH_BUFFER_BIT) && rf.depth_tex && df.depth_tex &&
        g_ctx.textures.count(rf.depth_tex) && g_ctx.textures.count(df.depth_tex)) {
        VkTexObj& src = g_ctx.textures[rf.depth_tex];
        VkTexObj& dst = g_ctx.textures[df.depth_tex];
        transition_subresource(cmd, src, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition_subresource(cmd, dst, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.extent = {static_cast<uint32_t>(std::min(src.width, dst.width)),
                       static_cast<uint32_t>(std::min(src.height, dst.height)), 1};
        g_ctx.vk.CmdCopyImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition_subresource(cmd, src, 0, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition_subresource(cmd, dst, 0, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void glDrawArrays(GLenum /*mode*/, GLint first, GLsizei count) {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t stride = 0;
    if (!prepare_draw(cmd, stride)) return;
    g_ctx.vk.CmdDraw(cmd, static_cast<uint32_t>(count), 1, static_cast<uint32_t>(first), 0);
}

void glDrawElements(GLenum /*mode*/, GLsizei count, GLenum type, const void* indices) {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t stride = 0;
    if (!prepare_draw(cmd, stride)) return;
    if (!g_ctx.bound_element_buf || !g_ctx.buffers.count(g_ctx.bound_element_buf)) return;
    const VkBufObj& ib = g_ctx.buffers[g_ctx.bound_element_buf];
    const VkBuffer b = ib.active_buf ? ib.active_buf : g_ctx.dummy_ubo_buf;
    const VkDeviceSize byte_off = ib.active_buf ? ib.active_offset : 0;
    const size_t idx_size = (type == GL_UNSIGNED_SHORT) ? 2 : 4;
    const uint32_t first_idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(indices) / idx_size);
    g_ctx.vk.CmdBindIndexBuffer(cmd, b, byte_off,
                                (type == GL_UNSIGNED_SHORT) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    g_ctx.vk.CmdDrawIndexed(cmd, static_cast<uint32_t>(count), 1, first_idx, 0, 0);
}

void glGenQueries(GLsizei n, GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) ids[i] = g_ctx.next_id++;
}
void glDeleteQueries(GLsizei, const GLuint*) {}
void glBeginQuery(GLenum, GLuint) {}
void glEndQuery(GLenum) {}
void glGetQueryObjectuiv(GLuint, GLenum, GLuint* p) { if (p) *p = 1; }
void glGetQueryObjectui64v(GLuint, GLenum, GLuint64* p) { if (p) *p = 0; }

void glDebugMessageCallback(GLDEBUGPROC, const void*) {}
void glDebugMessageControl(GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean) {}

}  // namespace vk_gl
}  // namespace me

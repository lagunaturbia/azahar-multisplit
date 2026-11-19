// SPDX-License-Identifier: GPL-3.0-or-later

#include "libretro.h"
#include "gpu_compositor.hpp"
#include "player_menu.hpp"
#include "vk_compositor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#include <fstream>
#endif
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#endif

namespace {
namespace fs = std::filesystem;

constexpr unsigned player_width = 400;
constexpr unsigned player_height = 480;
constexpr unsigned equal_width = 320;
constexpr unsigned max_players = 4;

#ifdef _WIN32
using Module = HMODULE;
constexpr char base_name[] = "azahar_libretro.dll";
#else
using Module = void*;
constexpr char base_name[] = "azahar_libretro.so";
#endif

struct Instance {
    Module module{};
    decltype(&retro_init) init{};
    decltype(&retro_deinit) deinit{};
    decltype(&retro_set_environment) set_env{};
    decltype(&retro_set_video_refresh) set_video{};
    decltype(&retro_set_audio_sample) set_audio{};
    decltype(&retro_set_audio_sample_batch) set_audio_batch{};
    decltype(&retro_set_input_poll) set_poll{};
    decltype(&retro_set_input_state) set_input{};
    decltype(&retro_load_game) load{};
    decltype(&retro_unload_game) unload{};
    decltype(&retro_run) run{};
    decltype(&retro_reset) reset{};
    decltype(&retro_get_system_av_info) get_av{};
    const retro_core_option_v2_definition* options{};
    bool (*init_local)(){};
    uint16_t (*host_local)(unsigned){};
    void (*set_equal_screens)(bool){};
    bool (*join_local)(uint16_t, unsigned){};
    void (*set_shader_context)(void* (*)(), bool (*)(void*), void (*)(void*), void (*)(void*)){};
    std::string save_dir;
    std::vector<uint32_t> software_frame;
    bool initialized{};
    bool game_loaded{};
    bool context_live{};
    retro_hw_render_callback* hw{};
    bool has_gpu_frame{};
};

#ifndef _WIN32
struct NativeSignalState {
    struct sigaction segv{};
    stack_t stack{};
    bool saved{};

    void save() {
        saved = sigaction(SIGSEGV, nullptr, &segv) == 0 && sigaltstack(nullptr, &stack) == 0;
    }

    void restore() {
        if (saved) {
            sigaction(SIGSEGV, &segv, nullptr);
            sigaltstack(&stack, nullptr);
            saved = false;
        }
    }
} native_signals;
#endif

std::array<Instance, max_players> instances;
std::array<unsigned, max_players> controller_devices{
    RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
std::vector<uint32_t> framebuffer;
fs::path session_dir;
fs::path save_root;
std::string content_path;
std::vector<uint32_t> menu_pixels;
unsigned menu_selection = 2;
unsigned drawn_selection{};
unsigned menu_previous_buttons = UINT32_MAX;
unsigned menu_buttons_to_release{};
unsigned active_players{};
retro_environment_t frontend_environment{};
retro_video_refresh_t frontend_video{};
retro_audio_sample_t frontend_audio{};
retro_audio_sample_batch_t frontend_audio_batch{};
retro_input_poll_t frontend_poll{};
retro_input_state_t frontend_input{};
retro_log_printf_t frontend_log{};
retro_system_timing timing{60.0, 32728.0};
bool loaded{};
enum class Renderer { Software, OpenGL, Vulkan };
Renderer renderer = Renderer::Software;
bool hardware() { return renderer != Renderer::Software; }
bool mouse_touch{};
bool direct_touch{};
bool context_failed{};
bool local_connected{};
unsigned video_scale = 1;
retro_hw_render_callback hardware_context{};
GpuCompositor gpu;
VulkanCompositor vulkan;
GpuCompositor menu_gpu;
VulkanCompositor menu_vulkan;
std::array<retro_hw_render_interface_vulkan, max_players> vk_interfaces{};
constexpr auto confirm_buttons = (1u << RETRO_DEVICE_ID_JOYPAD_A) |
                                 (1u << RETRO_DEVICE_ID_JOYPAD_START);

unsigned tile_width() { return active_players > 2 ? equal_width : player_width; }
retro_game_geometry geometry() {
    const unsigned width = active_players ? tile_width() * active_players * video_scale : PlayerMenu::width;
    const unsigned height = active_players ? player_height * video_scale : PlayerMenu::height;
    return {width, height, equal_width * max_players * video_scale, player_height * video_scale,
            static_cast<float>(width) / height};
}
const retro_hw_render_context_negotiation_interface_vulkan* vk_negotiation{};
retro_sensor_interface frontend_sensors{};
constexpr char renderer_option[] = "azahar_multisplit_renderer";

struct CoreOption {
    const char* key;
    const char* choices;
    const char* default_value;
};

constexpr CoreOption core_options[] = {
    {"citra_language_value", "3DS System Language (Restart); English|Spanish|Japanese|French|German|Italian|Dutch|Portuguese|Russian|Korean|Traditional Chinese|Simplified Chinese", "English"},
    {"citra_region_value", "3DS System Region (Restart); Auto|Japan|USA|Europe|Australia|China|Korea|Taiwan", "Auto"},
    {"citra_is_new_3ds", "3DS System Model (Restart); New 3DS|Old 3DS", "New 3DS"},
    {"citra_cpu_clock_percentage", "CPU Clock Speed (Restart); 100|25|50|75|125|150|175|200|225|250|275|300|325|350|375|400", "100"},
    {"citra_audio_emulation", "Audio Emulation (Restart); hle|lle|lle_multithread", "hle"},
    {"citra_analog_function", "Right Analog Function (Restart); c_stick_and_touchscreen|touchscreen_pointer|c_stick", "c_stick_and_touchscreen"},
    {"citra_analog_deadzone", "Analog Deadzone (Restart); 15|0|5|10|20|25|30|35", "15"},
    {"citra_enable_mouse_touchscreen", "Mouse Touchscreen Support (Restart); enabled|disabled", "enabled"},
    {"citra_enable_touch_touchscreen", "Touch Device Support (Restart); enabled|disabled", "enabled"},
    {"citra_enable_touch_pointer_timeout", "Touch Pointer Timeout (Restart); enabled|disabled", "enabled"},
    {"citra_enable_motion", "Motion Support (Restart); enabled|disabled", "enabled"},
    {"citra_motion_sensitivity", "Motion Sensitivity (Restart); 1.0|0.1|0.25|0.5|0.75|1.25|1.5|2.0", "1.0"},
    {"citra_async_shader_compilation", "Async Shader Compilation (Restart); enabled|disabled", "enabled"},
    {"citra_use_hw_shader", "Hardware Shaders (Restart); enabled|disabled", "enabled"},
    {"citra_use_shader_jit", "Shader JIT (Restart); enabled|disabled", "enabled"},
    {"citra_use_cpu_jit", "CPU JIT (Restart); enabled|disabled", "enabled"},
    {"citra_shaders_accurate_mul", "Accurate Shader Multiplication (Restart); enabled|disabled", "enabled"},
    {"citra_use_disk_shader_cache", "Hardware Shader Cache (Restart); enabled|disabled", "enabled"},
    {"citra_texture_filter", "Texture Filter (Restart); none|Anime4K Ultrafast|Bicubic|ScaleForce|xBRZ|MMPX", "none"},
    {"citra_texture_sampling", "Texture Sampling (Restart); GameControlled|NearestNeighbor|Linear", "GameControlled"},
    {"citra_custom_textures", "Custom Textures (Restart); disabled|enabled", "disabled"},
    {"citra_dump_textures", "Dump Textures (Restart); disabled|enabled", "disabled"},
};
constexpr const char* scale_options[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
constexpr char scale_option[] = "citra_resolution_factor";

struct Pointer {
    int16_t x{};
    int16_t y{};
    bool present{};
    bool pressed{};
    bool mouse_pressed{};
};
std::array<Pointer, max_players> pointers;
int touch_owner = -1;
bool was_mouse_down{};

void report(const std::string& message) {
    if (frontend_log)
        frontend_log(RETRO_LOG_ERROR, "[Azahar Multisplit] %s\n", message.c_str());
    if (frontend_environment) {
        retro_message notification{message.c_str(), 300};
        frontend_environment(RETRO_ENVIRONMENT_SET_MESSAGE, &notification);
    }
}

#ifdef _WIN32
void write_engine(const fs::path& destination) {
    HMODULE self{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&retro_api_version), &self))
        throw std::runtime_error("Could not locate the Azahar Multisplit module.");

    const HRSRC resource = FindResourceW(self, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    const HGLOBAL loaded_resource = resource ? LoadResource(self, resource) : nullptr;
    const auto* bytes = loaded_resource ? static_cast<const char*>(LockResource(loaded_resource))
                                        : nullptr;
    const auto size = resource ? SizeofResource(self, resource) : 0;
    if (!bytes || !size)
        throw std::runtime_error("The embedded Azahar engine is missing.");

    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    file.write(bytes, size);
    file.close();
    if (!file)
        throw std::runtime_error("Could not write the embedded Azahar engine to the temporary folder.");
}
#else
void write_engine(const fs::path& destination) {
    Dl_info self{};
    if (!dladdr(reinterpret_cast<void*>(&write_engine), &self) || !self.dli_fname)
        throw std::runtime_error("Could not locate the Linux test module.");
    const auto source = fs::absolute(self.dli_fname).parent_path() / base_name;
    fs::copy_file(source, destination);
}
#endif

fs::path create_session_directory() {
    const auto parent = fs::temp_directory_path();
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto path = parent / ("azahar-multisplit-" + std::to_string(seed) + "-" +
                              std::to_string(attempt));
        if (fs::create_directory(path))
            return path;
    }
    throw std::runtime_error("Could not create a temporary directory.");
}

fs::path instance_module_path(unsigned player) {
    const auto name = "azahar-P" + std::to_string(player + 1);
    return session_dir / (name + fs::path(base_name).extension().u8string());
}

Module open_module(const fs::path& path) {
#ifdef _WIN32
    auto module = LoadLibraryExW(path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        throw std::runtime_error("Could not load " + path.filename().u8string() +
                                 " (Windows error " + std::to_string(GetLastError()) + ").");
#else
    // Each player has a distinct ELF file and the engine binds its own symbols.
    // Keep all modules in the frontend's libc and Vulkan loader namespace.
    auto module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!module)
        throw std::runtime_error(dlerror());
#endif
    return module;
}

void close_module(Module module) {
#ifdef _WIN32
    FreeLibrary(module);
#else
    dlclose(module);
#endif
}

template <typename Function>
void bind_symbol(Module module, Function& function, const char* name) {
#ifdef _WIN32
    auto symbol = GetProcAddress(module, name);
#else
    auto symbol = dlsym(module, name);
#endif
    if (!symbol)
        throw std::runtime_error(std::string("Azahar is missing the entry point ") + name + ".");
    static_assert(sizeof(function) == sizeof(symbol));
    std::memcpy(&function, &symbol, sizeof(function));
}

const char* fixed_variable(const char* key) {
    if (std::strcmp(key, "citra_graphics_api") == 0)
        return renderer == Renderer::Vulkan ? "Vulkan" : hardware() ? "OpenGL" : "Software";
    if (std::strcmp(key, scale_option) == 0)
        return scale_options[video_scale - 1];

    for (const auto& option : core_options) {
        if (std::strcmp(key, option.key) == 0) {
            retro_variable value{option.key, nullptr};
            frontend_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &value);
            return value.value ? value.value : option.default_value;
        }
    }

    static constexpr std::pair<const char*, const char*> options[] = {
        {"citra_layout_option", "default"},
        {"citra_swap_screen", "Top"},
        {"citra_use_libretro_save_path", "LibRetro Default"},
    };
    for (const auto& [name, value] : options) {
        if (std::strcmp(key, name) == 0)
            return value;
    }
    return nullptr;
}

template <unsigned Player>
uintptr_t player_framebuffer() { return gpu.player_framebuffer(Player); }

retro_proc_address_t get_gl_proc(const char* symbol) {
    return hardware_context.get_proc_address ? hardware_context.get_proc_address(symbol) : nullptr;
}

Renderer preferred_renderer() {
    retro_variable choice{renderer_option, nullptr};
    frontend_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &choice);
    if (choice.value && std::strcmp(choice.value, "Software") == 0)
        return Renderer::Software;
    if (choice.value && std::strcmp(choice.value, "OpenGL") == 0)
        return Renderer::OpenGL;
    if (choice.value && std::strcmp(choice.value, "Vulkan") == 0)
        return Renderer::Vulkan;
    unsigned preferred = RETRO_HW_CONTEXT_NONE;
    if (frontend_environment(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred)) {
        if (preferred == RETRO_HW_CONTEXT_VULKAN)
            return Renderer::Vulkan;
        if (preferred == RETRO_HW_CONTEXT_OPENGL_CORE || preferred == RETRO_HW_CONTEXT_OPENGL)
            return Renderer::OpenGL;
    }
    return Renderer::Software;
}

template <unsigned Player>
void capture_vulkan(void*, const retro_vulkan_image* image, uint32_t count, const VkSemaphore*,
                    uint32_t queue_family) {
    vulkan.capture(Player, image, count, queue_family);
}

template <unsigned Player>
bool set_sensor(unsigned port, retro_sensor_action action, unsigned rate) {
    return port == 0 && frontend_sensors.set_sensor_state(Player, action, rate);
}

template <unsigned Player>
float get_sensor(unsigned port, unsigned id) {
    return port == 0 ? frontend_sensors.get_sensor_input(Player, id) : 0.0f;
}

void connect_local_room() {
    if (local_connected)
        return;
    const uint16_t port = instances[0].host_local(active_players);
    if (port) {
        unsigned player = 0;
        while (player < active_players && instances[player].join_local(port, player))
            ++player;
        if (player == active_players) {
            local_connected = true;
            if (frontend_log)
                frontend_log(RETRO_LOG_INFO, "[Azahar Multisplit] Local wireless room connected (%u players).\n",
                             active_players);
            return;
        }
    }
    report("Could not connect all 3DS consoles to the local wireless room.");
}

void context_destroy() {
    try {
        if (renderer == Renderer::Vulkan) {
            menu_vulkan.reset();
            vulkan.reset();
        }
        for (unsigned p = 0; p < active_players; ++p) {
            auto& instance = instances[p];
            if (!instance.context_live)
                continue;
            if (renderer == Renderer::OpenGL)
                gpu.activate(p);
            instance.context_live = false;
            instance.hw->context_destroy();
        }
        if (gpu.ready())
            gpu.activate_frontend();
    } catch (const std::exception& error) {
        report(error.what());
    }
    menu_gpu.reset();
    gpu.reset();
    vk_interfaces = {};
}

const retro_hw_render_interface_vulkan& vulkan_frontend() {
    const retro_hw_render_interface_vulkan* interface = nullptr;
    if (!frontend_environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &interface) ||
        !interface || interface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
        interface->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
        throw std::runtime_error("Select the Vulkan video driver before loading the game.");
    return *interface;
}

void activate_game_contexts() {
    const unsigned width = tile_width() * video_scale;
    const unsigned height = player_height * video_scale;
    if (renderer == Renderer::Vulkan) {
        const auto& interface = vulkan_frontend();
        vulkan.initialize(interface, width, height, active_players);
        vk_interfaces.fill(interface);
        vk_interfaces[0].set_image = capture_vulkan<0>;
        vk_interfaces[1].set_image = capture_vulkan<1>;
        vk_interfaces[2].set_image = capture_vulkan<2>;
        vk_interfaces[3].set_image = capture_vulkan<3>;
    } else {
        if (!hardware_context.get_current_framebuffer)
            throw std::runtime_error("RetroArch did not supply an OpenGL framebuffer.");
        gpu.initialize(get_gl_proc, width, height, active_players);
    }
    for (unsigned p = 0; p < active_players; ++p) {
        auto& instance = instances[p];
        if (renderer == Renderer::OpenGL)
            gpu.activate(p);
        instance.hw->context_reset();
        instance.context_live = true;
    }
    if (renderer == Renderer::OpenGL)
        gpu.activate_frontend();
    connect_local_room();
}

void context_reset() {
    if (!hardware())
        return;
    try {
        context_destroy();
        if (active_players) {
            activate_game_contexts();
        } else if (renderer == Renderer::Vulkan) {
            menu_vulkan.initialize(vulkan_frontend(), PlayerMenu::width, PlayerMenu::height, 0);
        } else {
            menu_gpu.initialize(get_gl_proc, PlayerMenu::width, PlayerMenu::height, 0);
        }
        context_failed = false;
    } catch (const std::exception& error) {
        context_failed = true;
        context_destroy();
        report(error.what());
    }
}

bool request_hardware_context() {
    hardware_context = {};
    hardware_context.context_reset = context_reset;
    hardware_context.context_destroy = context_destroy;
    if (renderer == Renderer::Vulkan) {
        if (!vk_negotiation)
            throw std::runtime_error("Azahar did not supply its Vulkan device negotiation.");
        hardware_context.context_type = RETRO_HW_CONTEXT_VULKAN;
        hardware_context.version_major = VK_MAKE_VERSION(1, 1, 0);
        if (!frontend_environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hardware_context))
            return false;
        if (!frontend_environment(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
            const_cast<retro_hw_render_context_negotiation_interface_vulkan*>(vk_negotiation)))
            throw std::runtime_error("RetroArch did not accept Vulkan device negotiation.");
        return true;
    }
    hardware_context.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    hardware_context.version_major = 4;
    hardware_context.version_minor = 3;
    hardware_context.bottom_left_origin = true;
    return frontend_environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hardware_context);
}

template <unsigned Player>
bool environment(unsigned command, void* data) {
    switch (command) {
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
        return renderer == Renderer::OpenGL;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        if (!hardware())
            return false;
        auto* callback = static_cast<retro_hw_render_callback*>(data);
        const auto expected = renderer == Renderer::Vulkan ? RETRO_HW_CONTEXT_VULKAN :
                                                             RETRO_HW_CONTEXT_OPENGL_CORE;
        if (callback->context_type != expected || !callback->context_reset || !callback->context_destroy)
            return false;
        if (renderer == Renderer::OpenGL) {
            callback->get_current_framebuffer = player_framebuffer<Player>;
            callback->get_proc_address = get_gl_proc;
        }
        instances[Player].hw = callback;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
        if (renderer != Renderer::Vulkan)
            return false;
        if constexpr (Player == 0)
            vk_negotiation = static_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(data);
        return true;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
        if (!vulkan.ready())
            return false;
        *static_cast<const retro_hw_render_interface_vulkan**>(data) = &vk_interfaces[Player];
        return true;
    case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
        if (!frontend_environment(command, &frontend_sensors) ||
            !frontend_sensors.set_sensor_state || !frontend_sensors.get_sensor_input)
            return false;
        *static_cast<retro_sensor_interface*>(data) = {set_sensor<Player>, get_sensor<Player>};
        return true;
    case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER: {
        auto& buffer = instances[Player].software_frame;
        auto& frame = *static_cast<retro_framebuffer*>(data);
        if (hardware() || buffer.empty() || !frame.width || !frame.height ||
            frame.width > tile_width() || frame.height > player_height)
            return false;
        frame.data = buffer.data();
        frame.pitch = frame.width * sizeof(uint32_t);
        frame.format = RETRO_PIXEL_FORMAT_XRGB8888;
        frame.memory_flags = RETRO_MEMORY_TYPE_CACHED;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = instances[Player].save_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto& variable = *static_cast<retro_variable*>(data);
        if (variable.key && std::strcmp(variable.key, "azahar_multisplit_audio_output") == 0) {
            variable.value = Player == 0 ? "enabled" : "disabled";
            return true;
        }
        variable.value = variable.key ? fixed_variable(variable.key) : nullptr;
        if (variable.key && !variable.value) {
            for (auto option = instances[Player].options; option && option->key; ++option) {
                if (std::strcmp(variable.key, option->key) == 0) {
                    variable.value = option->default_value;
                    break;
                }
            }
        }
        return variable.value != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *static_cast<const retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *static_cast<bool*>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned*>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        instances[Player].options = static_cast<const retro_core_options_v2*>(data)->definitions;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        instances[Player].options =
            static_cast<const retro_core_options_v2_intl*>(data)->us->definitions;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
    case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        return frontend_environment(command, data);
    default:
        // The frontend exposes one framebuffer, which belongs to the compositor.
        return false;
    }
}

template <unsigned Player>
void video(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (hardware()) {
        instances[Player].has_gpu_frame = data == RETRO_HW_FRAME_BUFFER_VALID;
        if (renderer == Renderer::Vulkan && instances[Player].has_gpu_frame)
            vulkan.set_size(Player, width, height);
        return;
    }
    const unsigned tile = tile_width();
    if (framebuffer.empty() || !data || !width || !height ||
        width > tile || height > player_height || pitch < width * sizeof(uint32_t))
        return;

    const auto* source = static_cast<const unsigned char*>(data);
    const auto total_width = tile * active_players;
    const auto horizontal_offset = (tile - width) / 2;
    const auto vertical_offset = (player_height - height) / 2;
    for (unsigned row = 0; row < player_height; ++row) {
        auto* destination = framebuffer.data() + row * total_width + Player * tile;
        if (width != tile || height != player_height)
            std::fill_n(destination, tile, 0);
        if (row >= vertical_offset && row - vertical_offset < height) {
            std::memcpy(destination + horizontal_offset, source + (row - vertical_offset) * pitch,
                        width * sizeof(uint32_t));
        }
    }
}

void poll_pointer() {
    pointers = {};
    if (!frontend_input)
        return;

    const auto point = [](unsigned id) {
        return frontend_input(0, RETRO_DEVICE_POINTER, 0, id);
    };
    const int x = point(RETRO_DEVICE_ID_POINTER_X);
    const int y = point(RETRO_DEVICE_ID_POINTER_Y);
    const bool touch_down = direct_touch && point(RETRO_DEVICE_ID_POINTER_PRESSED);
    const bool mouse_down = mouse_touch &&
                            frontend_input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
    const bool down = touch_down || mouse_down;

    int hit = -1;
    if (x >= -32767 && y >= -32767 && !point(RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN)) {
        constexpr int range = 32767;
        const int column_x = (x + range) * static_cast<int>(active_players);
        const unsigned player = std::min(static_cast<unsigned>(column_x / (2 * range)),
                                         active_players - 1);
        const int local_x = column_x - static_cast<int>(2 * player + 1) * range;
        const int touch_x_limit = range * equal_width / tile_width();
        const bool touches_screen = local_x >= -touch_x_limit && local_x < touch_x_limit && y >= 0;

        if (touches_screen) {
            hit = static_cast<int>(player);
            // Azahar ignores simultaneous zero coordinates. This remains the center pixel.
            pointers[player] = {static_cast<int16_t>(local_x == 0 && y == 0 ? 1 : local_x),
                                static_cast<int16_t>(y), true, false};
        }
    }

    if (!down || !was_mouse_down)
        touch_owner = down ? hit : -1;
    if (hit >= 0 && hit == touch_owner) {
        pointers[hit].pressed = touch_down;
        pointers[hit].mouse_pressed = mouse_down;
    }
    was_mouse_down = down;
}

template <unsigned Player>
int16_t input(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0 || !frontend_input)
        return 0;

    if (device == RETRO_DEVICE_POINTER && index == 0) {
        const auto& pointer = pointers[Player];
        switch (id) {
        case RETRO_DEVICE_ID_POINTER_X: return pointer.present ? pointer.x : -32768;
        case RETRO_DEVICE_ID_POINTER_Y: return pointer.present ? pointer.y : -32768;
        case RETRO_DEVICE_ID_POINTER_PRESSED: return pointer.pressed;
        case RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN: return !pointer.present;
        default: return 0;
        }
    }

    if (device == RETRO_DEVICE_MOUSE && id == RETRO_DEVICE_ID_MOUSE_LEFT)
        return pointers[Player].mouse_pressed;

    if ((device == RETRO_DEVICE_JOYPAD || device == RETRO_DEVICE_ANALOG) &&
        controller_devices[Player] != RETRO_DEVICE_NONE) {
        const int16_t value = frontend_input(Player, device, index, id);
        if constexpr (Player == 0) {
            if (device == RETRO_DEVICE_JOYPAD && menu_buttons_to_release) {
                if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
                    return static_cast<int16_t>(value & ~menu_buttons_to_release);
                if (id < 16 && (menu_buttons_to_release & (1u << id)))
                    return 0;
            }
        }
        return value;
    }
    return 0;
}

void input_already_polled() {}

void play_sample(int16_t left, int16_t right) {
    if (frontend_audio)
        frontend_audio(left, right);
}
size_t play_samples(const int16_t* samples, size_t frames) {
    return frontend_audio_batch ? frontend_audio_batch(samples, frames) : frames;
}
void drop_sample(int16_t, int16_t) {}
size_t drop_samples(const int16_t*, size_t frames) { return frames; }

template <unsigned Player>
void start_instance(const retro_game_info& game) {
    auto& instance = instances[Player];
    const auto player = std::string("P") + std::to_string(Player + 1);
    const auto save_path = save_root / player;
    fs::create_directories(save_path);
    instance.save_dir = save_path.generic_u8string();
    if (!hardware())
        instance.software_frame.resize(tile_width() * player_height);
    const auto copy = instance_module_path(Player);
    if constexpr (Player == 0) {
#ifndef _WIN32
        native_signals.save();
#endif
        write_engine(copy);
    }
    else
        fs::copy_file(instance_module_path(0), copy);
    instance.module = open_module(copy);

    auto module = instance.module;
    decltype(&retro_api_version) api_version{};
    bind_symbol(module, api_version, "retro_api_version");
    if (api_version() != RETRO_API_VERSION)
        throw std::runtime_error(player + ": incompatible Libretro API.");
    bind_symbol(module, instance.init, "retro_init");
    bind_symbol(module, instance.deinit, "retro_deinit");
    bind_symbol(module, instance.set_env, "retro_set_environment");
    bind_symbol(module, instance.set_video, "retro_set_video_refresh");
    bind_symbol(module, instance.set_audio, "retro_set_audio_sample");
    bind_symbol(module, instance.set_audio_batch, "retro_set_audio_sample_batch");
    bind_symbol(module, instance.set_poll, "retro_set_input_poll");
    bind_symbol(module, instance.set_input, "retro_set_input_state");
    bind_symbol(module, instance.load, "retro_load_game");
    bind_symbol(module, instance.unload, "retro_unload_game");
    bind_symbol(module, instance.run, "retro_run");
    bind_symbol(module, instance.reset, "retro_reset");
    bind_symbol(module, instance.get_av, "retro_get_system_av_info");
    bind_symbol(module, instance.init_local, "azahar_multisplit_init_local");
    bind_symbol(module, instance.host_local, "azahar_multisplit_host_local");
    bind_symbol(module, instance.join_local, "azahar_multisplit_join_local");
    bind_symbol(module, instance.set_equal_screens, "azahar_multisplit_set_equal_screens");
    bind_symbol(module, instance.set_shader_context, "azahar_multisplit_set_shader_context");
    instance.set_shader_context(create_shader_worker_context, make_shader_worker_current,
                                clear_shader_worker_current, destroy_shader_worker_context);

    instance.set_env(environment<Player>);
    instance.set_video(video<Player>);
    instance.set_input(input<Player>);
    instance.set_poll(input_already_polled);
    instance.set_audio(Player == 0 ? play_sample : drop_sample);
    instance.set_audio_batch(Player == 0 ? play_samples : drop_samples);
    instance.init();
    instance.initialized = true;
    if (active_players > 2)
        instance.set_equal_screens(true);
    if (!instance.init_local())
        throw std::runtime_error(player + ": Could not initialize the local wireless room.");
    if (!instance.load(&game))
        throw std::runtime_error(player + ": Azahar could not load the game. Check the RetroArch log.");
    instance.game_loaded = true;
    if (hardware() && !instance.hw)
        throw std::runtime_error(player + ": Azahar did not initialize the selected renderer.");
}

void close_instances() {
    loaded = false;
    context_destroy();
    for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
        if (it->game_loaded)
            it->unload();
        if (it->initialized)
            it->deinit();
    }
#ifndef _WIN32
    // Dynarmic installs process-wide signal handlers on Linux.
    native_signals.restore();
#endif
    for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
        if (it->module)
            close_module(it->module);
        *it = {};
    }
    framebuffer.clear();
    menu_pixels.clear();
    menu_selection = 2;
    drawn_selection = 0;
    menu_previous_buttons = UINT32_MAX;
    menu_buttons_to_release = 0;
    active_players = 0;
    save_root.clear();
    content_path.clear();
    pointers = {};
    touch_owner = -1;
    was_mouse_down = false;
    renderer = Renderer::Software;
    vk_negotiation = nullptr;
    context_failed = false;
    local_connected = false;
    video_scale = 1;
    if (!session_dir.empty()) {
        std::error_code error;
        // This directory was freshly created. Remove the engine copies owned by the core.
        for (unsigned p = 0; p < max_players; ++p)
            fs::remove(instance_module_path(p), error);
        fs::remove(session_dir, error);
        session_dir.clear();
    }
}

unsigned player_one_buttons() {
    if (!frontend_input || controller_devices[0] == RETRO_DEVICE_NONE)
        return 0;
    unsigned buttons = 0;
    for (const unsigned id : {RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
                              RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_START}) {
        if (frontend_input(0, RETRO_DEVICE_JOYPAD, 0, id))
            buttons |= 1u << id;
    }
    return buttons;
}

void start_players() {
    active_players = menu_selection;
    const retro_game_info game{content_path.c_str(), nullptr, 0, nullptr};
    if (active_players > 2)
        instances[0].set_equal_screens(true);
    if (!hardware())
        framebuffer.assign(tile_width() * active_players * player_height, 0);
    start_instance<1>(game);
    if (active_players >= 3)
        start_instance<2>(game);
    if (active_players >= 4)
        start_instance<3>(game);
    for (unsigned p = 1; p < active_players; ++p) {
        for (unsigned other = 0; other < p; ++other) {
            if (instances[p].module == instances[other].module)
                throw std::runtime_error("Could not isolate the Azahar modules.");
        }
    }
    if (hardware())
        activate_game_contexts();
    else
        connect_local_room();
    if (frontend_log)
        frontend_log(RETRO_LOG_INFO, "[Azahar Multisplit] %u instances, %s. Audio enabled for P1.\n",
                     active_players, renderer == Renderer::Vulkan ? "Vulkan GPU compositor" :
                     hardware() ? "OpenGL 4.3 GPU compositor" : "Software");

    auto update = geometry();
    if (!frontend_environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &update))
        report("RetroArch did not accept the updated splitscreen geometry.");
}

bool run_player_menu() {
    const unsigned buttons = player_one_buttons();
    const unsigned pressed = buttons & ~menu_previous_buttons;
    menu_previous_buttons = buttons;
    if ((pressed & (1u << RETRO_DEVICE_ID_JOYPAD_LEFT)) && menu_selection > 2)
        --menu_selection;
    if ((pressed & (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)) && menu_selection < max_players)
        ++menu_selection;
    if (pressed & confirm_buttons) {
        menu_buttons_to_release = buttons & confirm_buttons;
        start_players();
        menu_pixels.clear();
        return true;
    }
    if (drawn_selection != menu_selection) {
        PlayerMenu::draw(menu_pixels, menu_selection);
        drawn_selection = menu_selection;
    }
    if (renderer == Renderer::Vulkan) {
        if (menu_vulkan.draw_menu(menu_pixels.data(), menu_selection))
            frontend_video(RETRO_HW_FRAME_BUFFER_VALID, PlayerMenu::width, PlayerMenu::height, 0);
    } else if (renderer == Renderer::OpenGL) {
        if (menu_gpu.ready()) {
            menu_gpu.draw_menu(hardware_context.get_current_framebuffer, menu_pixels.data(), menu_selection);
            frontend_video(RETRO_HW_FRAME_BUFFER_VALID, PlayerMenu::width, PlayerMenu::height, 0);
        }
    } else {
        frontend_video(menu_pixels.data(), PlayerMenu::width, PlayerMenu::height,
                       PlayerMenu::width * sizeof(uint32_t));
    }
    return false;
}

} // namespace

void retro_set_environment(retro_environment_t callback) {
    frontend_environment = callback;
    static const auto options = [] {
        std::array<retro_variable, std::size(core_options) + 3> values{};
        values[0] = {renderer_option, "Renderer (Restart); Auto|OpenGL|Vulkan|Software"};
        values[1] = {scale_option, "Internal Resolution (Restart); 1|2|3|4|5|6|7|8"};
        for (unsigned i = 0; i < std::size(core_options); ++i)
            values[i + 2] = {core_options[i].key, core_options[i].choices};
        return values;
    }();
    frontend_environment(RETRO_ENVIRONMENT_SET_VARIABLES,
                         const_cast<retro_variable*>(options.data()));
    static constexpr retro_controller_description joypad[] = {{"RetroPad", RETRO_DEVICE_JOYPAD}};
    static const retro_controller_info controllers[] = {{joypad, 1}, {joypad, 1}, {joypad, 1}, {joypad, 1}, {}};
    frontend_environment(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,
                         const_cast<retro_controller_info*>(controllers));
}
void retro_set_video_refresh(retro_video_refresh_t callback) { frontend_video = callback; }
void retro_set_audio_sample(retro_audio_sample_t callback) { frontend_audio = callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
    frontend_audio_batch = callback;
}
void retro_set_input_poll(retro_input_poll_t callback) { frontend_poll = callback; }
void retro_set_input_state(retro_input_state_t callback) { frontend_input = callback; }

void retro_init() {
    retro_log_callback callback{};
    if (frontend_environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &callback))
        frontend_log = callback.log;
}

void retro_deinit() {
    close_instances();
    frontend_log = nullptr;
}

unsigned retro_api_version() { return RETRO_API_VERSION; }

void retro_get_system_info(retro_system_info* info) {
    *info = {"Azahar Multisplit", "1.0", "3ds|3dsx|cci|cxi|app|elf|axf|z3dsx|zcci|zcxi",
             true, false};
}

void retro_get_system_av_info(retro_system_av_info* info) {
    *info = {geometry(), timing};
}

bool retro_load_game(const retro_game_info* game) {
    close_instances();
    if (!game || !game->path || !*game->path)
        return false;

    try {
        const char* save_directory{};
        frontend_environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_directory);
        if (!save_directory || !*save_directory)
            frontend_environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &save_directory);
        if (!save_directory || !*save_directory)
            throw std::runtime_error("Set a Save Files directory in RetroArch.");

        const auto player_saves = fs::u8path(save_directory) / "Azahar-Multisplit";
        save_root = player_saves;
        auto format = RETRO_PIXEL_FORMAT_XRGB8888;
        if (!frontend_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
            throw std::runtime_error("Frontend does not support XRGB8888.");

        mouse_touch = std::strcmp(fixed_variable("citra_enable_mouse_touchscreen"), "enabled") == 0;
        direct_touch = std::strcmp(fixed_variable("citra_enable_touch_touchscreen"), "enabled") == 0;
        renderer = preferred_renderer();
        if (hardware()) {
            retro_variable option{scale_option, nullptr};
            if (frontend_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &option) && option.value) {
                for (unsigned i = 0; i < std::size(scale_options); ++i)
                    if (std::strcmp(option.value, scale_options[i]) == 0)
                        video_scale = i + 1;
            }
        }
        // P1 supplies Vulkan negotiation without booting the game until the selector is confirmed.
        session_dir = create_session_directory();
        start_instance<0>(*game);
        if (hardware() && !request_hardware_context()) {
            if (renderer == Renderer::Vulkan)
                throw std::runtime_error("RetroArch rejected Vulkan. Select the Vulkan video driver.");
            if (frontend_log)
                frontend_log(RETRO_LOG_WARN, "[Azahar Multisplit] OpenGL unavailable, using Software.\n");
            close_instances();
            session_dir = create_session_directory();
            save_root = player_saves;
            start_instance<0>(*game);
        }
        retro_system_av_info upstream{};
        instances[0].get_av(&upstream);
        timing = upstream.timing;
        content_path = game->path;
        loaded = true;
        return true;
    } catch (const std::exception& error) {
        report(error.what());
        close_instances();
        return false;
    }
}

void retro_unload_game() { close_instances(); }
bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }
void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (port < controller_devices.size())
        controller_devices[port] = device;
}

void retro_run() {
    if (!loaded)
        return;
    if (frontend_poll)
        frontend_poll();
    if (context_failed) {
        const auto output = geometry();
        frontend_video(nullptr, output.base_width, output.base_height, 0);
        return;
    }
    try {
        if (!active_players) {
            if ((renderer == Renderer::Vulkan && !menu_vulkan.ready()) ||
                (renderer == Renderer::OpenGL && !menu_gpu.ready()) || !run_player_menu())
                return;
        } else if ((renderer == Renderer::Vulkan && !vulkan.ready()) ||
                   (renderer == Renderer::OpenGL && !gpu.ready())) {
            return;
        }
        if (menu_buttons_to_release)
            menu_buttons_to_release &= player_one_buttons();
        poll_pointer();

        const auto output = geometry();
        if (!hardware()) {
            for (unsigned p = 0; p < active_players; ++p)
                instances[p].run();
            frontend_video(framebuffer.data(), output.base_width, output.base_height,
                           output.base_width * sizeof(uint32_t));
            return;
        }
        if (renderer == Renderer::Vulkan) {
            for (unsigned p = 0; p < active_players; ++p)
                instances[p].run();
            vulkan.compose();
        } else {
            for (unsigned p = 0; p < active_players; ++p) {
                auto& instance = instances[p];
                gpu.activate(p);
                gpu.start_player_frame();
                instance.has_gpu_frame = false;
                instance.run();
                if (instance.has_gpu_frame)
                    gpu.finish_player_frame(p);
            }
            gpu.activate_frontend();
            gpu.compose(hardware_context.get_current_framebuffer);
        }
        frontend_video(RETRO_HW_FRAME_BUFFER_VALID, output.base_width, output.base_height, 0);
    } catch (const std::exception& error) {
        context_failed = true;
        report(error.what());
    }
}

void retro_reset() {
    if (!loaded || !active_players || context_failed || (renderer == Renderer::OpenGL && !gpu.ready()) ||
        (renderer == Renderer::Vulkan && !vulkan.ready()))
        return;
    try {
        std::fill(framebuffer.begin(), framebuffer.end(), 0);
        vulkan.clear_sources();
        for (unsigned p = 0; p < active_players; ++p) {
            if (renderer == Renderer::OpenGL)
                gpu.activate(p);
            instances[p].reset();
        }
        if (renderer == Renderer::OpenGL)
            gpu.activate_frontend();
    } catch (const std::exception& error) {
        context_failed = true;
        context_destroy();
        report(error.what());
    }
}

unsigned retro_get_region() { return RETRO_REGION_NTSC; }
size_t retro_serialize_size() { return 0; }
bool retro_serialize(void*, size_t) { return false; }
bool retro_unserialize(const void*, size_t) { return false; }
void retro_cheat_reset() {}
void retro_cheat_set(unsigned, bool, const char*) {}
void* retro_get_memory_data(unsigned) { return nullptr; }
size_t retro_get_memory_size(unsigned) { return 0; }

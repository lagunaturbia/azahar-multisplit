// SPDX-License-Identifier: GPL-3.0-or-later

#include "libretro.h"
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
extern "C" {
void* eglGetPlatformDisplay(unsigned, void*, const intptr_t*);
unsigned eglInitialize(void*, int*, int*);
unsigned eglBindAPI(unsigned);
unsigned eglChooseConfig(void*, const int*, void**, int, int*);
void* eglCreatePbufferSurface(void*, void*, const int*);
void* eglCreateContext(void*, void*, void*, const int*);
unsigned eglMakeCurrent(void*, void*, void*, void*);
void (*eglGetProcAddress(const char*))();
unsigned eglDestroyContext(void*, void*);
unsigned eglDestroySurface(void*, void*);
unsigned eglTerminate(void*);
}
#endif

namespace {
std::string save_dir;
std::array<unsigned, 2> queries{};
std::array<uint64_t, 2> hashes{};
std::array<unsigned, 2> color_pixels{};
std::array<unsigned, 2> top_color_pixels{};
unsigned frames{};
unsigned polls{};
unsigned audio_batches{};
unsigned menu_frames{};
bool in_selector{};
bool confirm_menu{};
bool use_gl{};
bool expect_compiled_shader{};
bool disable_async_shaders{};
unsigned scale = 1;
unsigned output_width = 800;
unsigned output_height = 480;
std::atomic<unsigned> joined_rooms{};
std::atomic<unsigned> shader_workers{};
std::atomic<unsigned> compiled_shaders{};
retro_hw_render_callback* hw{};
unsigned output_fbo{};
std::vector<uint32_t> readback;
#ifndef _WIN32
void* egl_display{};
void* egl_surface{};
void* egl_context{};
unsigned output_texture{};

template<typename T>
T gl(const char* name) { return reinterpret_cast<T>(eglGetProcAddress(name)); }

bool open_gl_frontend() {
    egl_display = eglGetPlatformDisplay(0x31DD, nullptr, nullptr); // EGL_PLATFORM_SURFACELESS_MESA
    if (!egl_display || !eglInitialize(egl_display, nullptr, nullptr) || !eglBindAPI(0x30A2))
        return false;
    const int config_attribs[] = {0x3033, 1, 0x3040, 8, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3038};
    int count{};
    void* config{};
    if (!eglChooseConfig(egl_display, config_attribs, &config, 1, &count) || !count)
        return false;
    const int surface_attribs[] = {0x3057, static_cast<int>(output_width), 0x3056,
                                  static_cast<int>(output_height), 0x3038};
    egl_surface = eglCreatePbufferSurface(egl_display, config, surface_attribs);
    const int context_attribs[] = {0x3098, 4, 0x30FB, 3, 0x30FD, 1, 0x3038};
    egl_context = eglCreateContext(egl_display, config, nullptr, context_attribs);
    if (!egl_surface || !egl_context ||
        !eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context))
        return false;

    gl<void(*)(int,unsigned*)>("glGenTextures")(1, &output_texture);
    gl<void(*)(unsigned,unsigned)>("glBindTexture")(0x0DE1, output_texture);
    gl<void(*)(unsigned,int,unsigned,int,int)>("glTexStorage2D")(0x0DE1, 1, 0x8058, output_width, output_height);
    gl<void(*)(int,unsigned*)>("glGenFramebuffers")(1, &output_fbo);
    gl<void(*)(unsigned,unsigned)>("glBindFramebuffer")(0x8D40, output_fbo);
    gl<void(*)(unsigned,unsigned,unsigned,unsigned,int)>("glFramebufferTexture2D")(
        0x8D40, 0x8CE0, 0x0DE1, output_texture, 0);
    if (gl<unsigned(*)(unsigned)>("glCheckFramebufferStatus")(0x8D40) != 0x8CD5)
        return false;
    gl<void(*)(unsigned,unsigned)>("glBindFramebuffer")(0x8D40, 0);
    readback.resize(output_width * output_height);
    return true;
}

void close_gl_frontend() {
    gl<void(*)(int,const unsigned*)>("glDeleteFramebuffers")(1, &output_fbo);
    gl<void(*)(int,const unsigned*)>("glDeleteTextures")(1, &output_texture);
    eglMakeCurrent(egl_display, nullptr, nullptr, nullptr);
    eglDestroyContext(egl_display, egl_context);
    eglDestroySurface(egl_display, egl_surface);
    eglTerminate(egl_display);
}
#endif

void logger(retro_log_level level, const char* format, ...) {
    if (level < RETRO_LOG_INFO)
        return;
    va_list args;
    va_start(args, format);
    char line[4096]{};
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (std::strstr(line, "Local wireless room connected"))
        ++joined_rooms;
    if (std::strstr(line, "Asynchronous fragment shader compilation is enabled"))
        ++shader_workers;
    if (std::strstr(line, "First fragment shader compiled on the background thread"))
        ++compiled_shaders;
    std::fputs(line, stderr);
}
bool environment(unsigned command, void* data) {
    switch (command) {
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto& variable = *static_cast<retro_variable*>(data);
        if (std::strcmp(variable.key, "citra_resolution_factor") == 0 && scale == 2) {
            variable.value = "2";
            return true;
        }
        if (std::strcmp(variable.key, "citra_async_shader_compilation") == 0 && disable_async_shaders) {
            variable.value = "disabled";
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *static_cast<unsigned*>(data) = use_gl ? RETRO_HW_CONTEXT_OPENGL_CORE : RETRO_HW_CONTEXT_NONE;
        return true;
#ifndef _WIN32
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
        if (!use_gl)
            return false;
        hw = static_cast<retro_hw_render_callback*>(data);
        hw->get_current_framebuffer = +[]() -> uintptr_t { return output_fbo; };
        hw->get_proc_address = eglGetProcAddress;
        return hw->context_type == RETRO_HW_CONTEXT_OPENGL_CORE;
#endif
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = logger;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char**>(data) = save_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        std::fprintf(stderr, "Message: %s\n", static_cast<retro_message*>(data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return static_cast<const retro_game_geometry*>(data)->base_width == output_width &&
               static_cast<const retro_game_geometry*>(data)->base_height == output_height;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    default:
        return false;
    }
}
void video(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (width != (in_selector ? 800u : output_width) ||
        height != (in_selector ? 480u : output_height) || !data)
        std::abort();
#ifndef _WIN32
    if (use_gl) {
        if (data != RETRO_HW_FRAME_BUFFER_VALID || pitch != 0)
            std::abort();
        gl<void(*)(unsigned,unsigned)>("glBindFramebuffer")(0x8CA8, output_fbo);
        gl<void(*)(int,int,int,int,unsigned,unsigned,void*)>("glReadPixels")(
            0, 0, width, height, 0x80E1, 0x1401, readback.data());
        gl<void(*)(unsigned,unsigned)>("glBindFramebuffer")(0x8CA8, 0);
        const auto error = gl<unsigned(*)()>("glGetError")();
        if (error) {
            std::fprintf(stderr, "OpenGL error 0x%x before reading two screens\n", error);
            std::abort();
        }
        data = readback.data();
    } else
#endif
    if (pitch != width * sizeof(uint32_t)) {
        std::abort();
    }
    const auto* pixels = static_cast<const uint32_t*>(data);
    if (in_selector) {
        const unsigned row = use_gl ? height - 1 - 210 : 210;
        if ((pixels[row * width + 138] & 0xffffff) != 0x71e0d2 ||
            (pixels[row * width + 350] & 0xffffff) != 0x1c2837)
            std::abort();
        ++menu_frames;
        return;
    }
    for (unsigned player = 0; player < 2; ++player) {
        uint64_t hash = 14695981039346656037ull;
        unsigned non_black = 0;
        unsigned top_non_black = 0;
        for (unsigned row = 0; row < height; ++row) {
            const unsigned source_row = use_gl ? height - 1 - row : row;
            auto* first = pixels + source_row * width + player * width / 2;
            for (unsigned x = 0; x < width / 2; ++x) {
                const auto color = first[x] & 0xffffff;
                hash = (hash ^ color) * 1099511628211ull;
                non_black += color != 0;
                top_non_black += row < height / 2 && color != 0;
            }
        }
        color_pixels[player] = non_black;
        top_color_pixels[player] = top_non_black;
        hashes[player] = hash;
    }
    ++frames;
}
void poll() { ++polls; }
int16_t input(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port >= 2)
        std::abort();
    ++queries[port];
    if (confirm_menu && port == 0 && device == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_A)
        return 1;
    if (port == 0 && device == RETRO_DEVICE_JOYPAD && frames >= 100 && frames < 105) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            return 1 << RETRO_DEVICE_ID_JOYPAD_L3;
        if (id == RETRO_DEVICE_ID_JOYPAD_L3)
            return 1;
    }
    return 0;
}
void audio(int16_t, int16_t) {}
size_t audio_batch(const int16_t*, size_t count) { ++audio_batches; return count; }

#ifdef _WIN32
using Module = HMODULE;
Module open(const char* path) { return LoadLibraryW(std::filesystem::u8path(path).c_str()); }
void close(Module module) { FreeLibrary(module); }
#else
using Module = void*;
Module open(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void close(Module module) { dlclose(module); }
#endif

template <typename T>
T symbol(Module module, const char* name) {
#ifdef _WIN32
    auto address = GetProcAddress(module, name);
#else
    auto address = dlsym(module, name);
#endif
    if (!address) {
        std::fprintf(stderr, "Missing %s\n", name);
        std::abort();
    }
    T fn{};
    std::memcpy(&fn, &address, sizeof(fn));
    return fn;
}
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5)
        return 1;
    use_gl = argc == 5 && std::strncmp(argv[4], "opengl", 6) == 0;
    scale = argc == 5 && std::strcmp(argv[4], "opengl2") == 0 ? 2 : 1;
    expect_compiled_shader = argc == 5 && std::strcmp(argv[4], "opengl-shaders") == 0;
    disable_async_shaders = argc == 5 && std::strcmp(argv[4], "opengl-sync") == 0;
    output_width = 800 * scale;
    output_height = 480 * scale;
#ifndef _WIN32
    if (use_gl && !open_gl_frontend()) {
        std::fprintf(stderr, "Failed to initialize surfaceless OpenGL\n");
        return 8;
    }
#endif
    save_dir = std::filesystem::u8path(argv[3]).generic_u8string();
    std::filesystem::create_directories(std::filesystem::u8path(save_dir));
    auto module = open(argv[1]);
    if (!module) {
#ifdef _WIN32
        std::fprintf(stderr, "DLL failed to load: %lu\n", GetLastError());
#else
        std::fprintf(stderr, "SO failed to load: %s\n", dlerror());
#endif
        return 2;
    }
#define WRAP(name) symbol<decltype(&retro_##name)>(module, "retro_" #name)
    WRAP(set_environment)(environment);
    WRAP(set_video_refresh)(video);
    WRAP(set_input_poll)(poll);
    WRAP(set_input_state)(input);
    WRAP(set_audio_sample)(audio);
    WRAP(set_audio_sample_batch)(audio_batch);
    WRAP(init)();

    // A failed load must leave both instances ready for the next load.
    retro_game_info missing{"missing.3dsx", nullptr, 0, nullptr};
    if (WRAP(load_game)(&missing))
        return 3;

    const retro_game_info game{argv[2], nullptr, 0, nullptr};
    const auto run = WRAP(run);
    for (unsigned round = 0; round < 2; ++round) {
        if (!WRAP(load_game)(&game))
            return 4;
        if (use_gl) {
            if (!hw)
                return 9;
            hw->context_reset();
        }
        retro_system_av_info av{};
        WRAP(get_system_av_info)(&av);
        if (av.geometry.base_width != 800 || av.geometry.base_height != 480 ||
            av.geometry.max_width != 1280 * scale || av.geometry.max_height != output_height ||
            av.timing.fps <= 0 || av.timing.sample_rate <= 0)
            return 5;
        in_selector = true;
        confirm_menu = true;
        menu_frames = 0;
        run();
        confirm_menu = false;
        run();
        if (menu_frames != 2)
            return 10;
        frames = polls = audio_batches = 0;
        queries = {};
        unsigned both_visible = 0;
        bool swap_observed = false;
        std::array<unsigned, 2> colors_before{};
        in_selector = false;
        for (unsigned i = 0; i < 150; ++i) {
            confirm_menu = i == 0;
            run();
            if (color_pixels[0] > 100 && color_pixels[1] > 100)
                ++both_visible;
            if (i == 99)
                colors_before = color_pixels;
            if (i >= 103 && top_color_pixels[0] < top_color_pixels[1] / 2 &&
                color_pixels[0] > 100 && hashes[0] != hashes[1])
                swap_observed = true;
        }
        std::printf("round=%u fps=%.4f rate=%.0f frames=%u polls=%u input=%u,%u "
                    "audio_batches=%u both_visible=%u swap=%d colors_before=%u,%u colors_after=%u,%u top_after=%u,%u\n",
                    round + 1, av.timing.fps, av.timing.sample_rate, frames, polls, queries[0],
                    queries[1], audio_batches, both_visible, swap_observed, colors_before[0],
                    colors_before[1], color_pixels[0], color_pixels[1], top_color_pixels[0],
                    top_color_pixels[1]);
        std::fflush(stdout);
        if (use_gl && round == 1)
            hw->context_destroy();
        WRAP(unload_game)();
        if (frames != 150 || polls != 150 || !queries[0] || !queries[1] ||
            both_visible < 15 || (!expect_compiled_shader && !swap_observed) ||
            joined_rooms < round + 1 ||
            (use_gl && !disable_async_shaders && shader_workers < (round + 1) * 2)) {
            WRAP(deinit)();
            return 6;
        }
    }
    WRAP(deinit)();
    std::printf("rooms=%u workers=%u compiled=%u\n", joined_rooms.load(),
                shader_workers.load(), compiled_shaders.load());
    close(module);
#ifndef _WIN32
    if (use_gl)
        close_gl_frontend();
#endif
    if (expect_compiled_shader && compiled_shaders < 2)
        return 10;
    if (disable_async_shaders && (shader_workers != 0 || compiled_shaders != 0))
        return 11;
    return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu_compositor.hpp"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define GL_CALL WINAPI

namespace {
using GlContext = HGLRC;

struct NativeContexts {
    HDC dc = wglGetCurrentDC();
    GlContext frontend = wglGetCurrentContext();

    GlContext create() const {
        if (!dc || !frontend)
            throw std::runtime_error("OpenGL requires a current WGL context. Select glcore with WGL.");
        using Create = HGLRC (WINAPI*)(HDC, HGLRC, const int*);
        const auto proc = wglGetProcAddress("wglCreateContextAttribsARB");
        if (!proc || reinterpret_cast<uintptr_t>(proc) <= 3 ||
            reinterpret_cast<uintptr_t>(proc) == UINTPTR_MAX)
            throw std::runtime_error("WGL_ARB_create_context is unavailable.");
        auto create_context = reinterpret_cast<Create>(proc);
        const int attributes[] = {0x2091, 4, 0x2092, 3, 0x9126, 1, 0};
        return create_context(dc, frontend, attributes);
    }

    bool activate(GlContext context) const { return wglMakeCurrent(dc, context) != 0; }
    static void destroy(GlContext context) { wglDeleteContext(context); }
};
}
#else
#define GL_CALL
// The Linux smoke test uses Mesa/EGL in a surfaceless session.
extern "C" {
void* eglGetCurrentDisplay();
void* eglGetCurrentContext();
void* eglGetCurrentSurface(int);
unsigned eglQueryContext(void*, void*, int, int*);
unsigned eglChooseConfig(void*, const int*, void**, int, int*);
unsigned eglBindAPI(unsigned);
void* eglCreateContext(void*, void*, void*, const int*);
unsigned eglMakeCurrent(void*, void*, void*, void*);
unsigned eglDestroyContext(void*, void*);
}

namespace {
using GlContext = void*;

struct NativeContexts {
    void* display = eglGetCurrentDisplay();
    void* draw = eglGetCurrentSurface(0x3059);
    void* read = eglGetCurrentSurface(0x305A);
    GlContext frontend = eglGetCurrentContext();

    GlContext create() const {
        int config_id{}, count{};
        void* config{};
        if (!display || !frontend || !eglQueryContext(display, frontend, 0x3028, &config_id))
            throw std::runtime_error("OpenGL requires a current EGL context for this build.");
        const int config_attributes[] = {0x3028, config_id, 0x3038};
        if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count == 0 ||
            !eglBindAPI(0x30A2))
            throw std::runtime_error("Could not reuse the frontend EGL configuration.");
        const int attributes[] = {0x3098, 4, 0x30FB, 3, 0x30FD, 1, 0x3038};
        return eglCreateContext(display, config, frontend, attributes);
    }

    bool activate(GlContext context) const {
        return eglMakeCurrent(display, draw, read, context) != 0;
    }
    void destroy(GlContext context) const { eglDestroyContext(display, context); }
};
}
#endif

namespace {
using Enum = unsigned;
using Name = unsigned;
using Sync = struct GlSync*;

constexpr Enum texture_2d = 0x0DE1;
constexpr Enum rgba8 = 0x8058;
constexpr Enum framebuffer = 0x8D40;
constexpr Enum read_framebuffer = 0x8CA8;
constexpr Enum draw_framebuffer = 0x8CA9;
constexpr Enum read_framebuffer_binding = 0x8CAA;
constexpr Enum draw_framebuffer_binding = 0x8CA6;
constexpr Enum framebuffer_complete = 0x8CD5;
constexpr Enum color_attachment = 0x8CE0;
constexpr Enum color = 0x1800;
constexpr Enum color_buffer_bit = 0x4000;
constexpr Enum nearest = 0x2600;
constexpr Enum scissor_test = 0x0C11;
constexpr Enum sync_gpu_commands_complete = 0x9117;

struct Gl {
    void (GL_CALL* GenTextures)(int, Name*);
    void (GL_CALL* DeleteTextures)(int, const Name*);
    void (GL_CALL* BindTexture)(Enum, Name);
    void (GL_CALL* TexStorage2D)(Enum, int, Enum, int, int);
    void (GL_CALL* TexSubImage2D)(Enum, int, int, int, int, int, Enum, Enum, const void*);
    void (GL_CALL* BindBuffer)(Enum, Name);
    void (GL_CALL* PixelStorei)(Enum, int);
    void (GL_CALL* GenFramebuffers)(int, Name*);
    void (GL_CALL* DeleteFramebuffers)(int, const Name*);
    void (GL_CALL* BindFramebuffer)(Enum, Name);
    void (GL_CALL* FramebufferTexture2D)(Enum, Enum, Enum, Name, int);
    Enum (GL_CALL* CheckFramebufferStatus)(Enum);
    void (GL_CALL* ClearBufferfv)(Enum, int, const float*);
    void (GL_CALL* BlitFramebuffer)(int, int, int, int, int, int, int, int, Enum, Enum);
    void (GL_CALL* GetIntegerv)(Enum, int*);
    unsigned char (GL_CALL* IsEnabled)(Enum);
    void (GL_CALL* Enable)(Enum);
    void (GL_CALL* Disable)(Enum);
    Sync (GL_CALL* FenceSync)(Enum, unsigned);
    void (GL_CALL* WaitSync)(Sync, unsigned, uint64_t);
    void (GL_CALL* DeleteSync)(Sync);
    void (GL_CALL* Flush)();

    explicit Gl(retro_hw_get_proc_address_t resolve) {
        if (!resolve)
            throw std::runtime_error("RetroArch did not supply an OpenGL function resolver.");
#define LOAD_GL(name)                                                                          \
        name = reinterpret_cast<decltype(name)>(resolve("gl" #name));                           \
        if (!name)                                                                              \
            throw std::runtime_error("Missing OpenGL function gl" #name ".");
        LOAD_GL(GenTextures)
        LOAD_GL(DeleteTextures)
        LOAD_GL(BindTexture)
        LOAD_GL(TexStorage2D)
        LOAD_GL(TexSubImage2D)
        LOAD_GL(BindBuffer)
        LOAD_GL(PixelStorei)
        LOAD_GL(GenFramebuffers)
        LOAD_GL(DeleteFramebuffers)
        LOAD_GL(BindFramebuffer)
        LOAD_GL(FramebufferTexture2D)
        LOAD_GL(CheckFramebufferStatus)
        LOAD_GL(ClearBufferfv)
        LOAD_GL(BlitFramebuffer)
        LOAD_GL(GetIntegerv)
        LOAD_GL(IsEnabled)
        LOAD_GL(Enable)
        LOAD_GL(Disable)
        LOAD_GL(FenceSync)
        LOAD_GL(WaitSync)
        LOAD_GL(DeleteSync)
        LOAD_GL(Flush)
#undef LOAD_GL
    }
};

struct PlayerTarget {
    GlContext context{};
    Name texture{};
    Name render_fbo{};
    Name read_fbo{};
    Sync render_fence{};
};

void attach_color(const Gl& gl, Enum target, Name fbo, Name texture) {
    gl.BindFramebuffer(target, fbo);
    gl.FramebufferTexture2D(target, color_attachment, texture_2d, texture, 0);
    if (gl.CheckFramebufferStatus(target) != framebuffer_complete)
        throw std::runtime_error("Could not create a complete OpenGL framebuffer.");
}
}

struct GpuCompositor::State {
    NativeContexts contexts;
    Gl gl;
    int width;
    int height;
    unsigned count;
    std::array<PlayerTarget, 4> players{};
    Name menu_texture{};
    Name menu_fbo{};
    unsigned menu_selection{};
    Sync composition_fence{};

    State(retro_hw_get_proc_address_t resolver, unsigned w, unsigned h, unsigned players)
        : gl(resolver), width(static_cast<int>(w)), height(static_cast<int>(h)), count(players) {}

    void activate(GlContext context) const {
        if (!contexts.activate(context))
            throw std::runtime_error("Could not switch the OpenGL context.");
    }

    void setup() {
        for (unsigned i = 0; i < count; ++i) {
            auto& player = players[i];
            player.context = contexts.create();
            if (!player.context)
                throw std::runtime_error("Could not create a shared OpenGL 4.3 context.");
            activate(player.context);
            gl.GenTextures(1, &player.texture);
            gl.BindTexture(texture_2d, player.texture);
            gl.TexStorage2D(texture_2d, 1, rgba8, width, height);
            gl.BindTexture(texture_2d, 0);
            gl.GenFramebuffers(1, &player.render_fbo);
            attach_color(gl, framebuffer, player.render_fbo, player.texture);
            constexpr float black[] = {0, 0, 0, 1};
            gl.ClearBufferfv(color, 0, black);
            gl.BindFramebuffer(framebuffer, 0);
            player.render_fence = gl.FenceSync(sync_gpu_commands_complete, 0);
            gl.Flush();

            activate(contexts.frontend);
            int old_read{};
            gl.GetIntegerv(read_framebuffer_binding, &old_read);
            gl.GenFramebuffers(1, &player.read_fbo);
            attach_color(gl, read_framebuffer, player.read_fbo, player.texture);
            gl.BindFramebuffer(read_framebuffer, old_read);
        }
    }

    ~State() {
        if (contexts.frontend && contexts.activate(contexts.frontend)) {
            if (menu_fbo)
                gl.DeleteFramebuffers(1, &menu_fbo);
            if (menu_texture)
                gl.DeleteTextures(1, &menu_texture);
            if (composition_fence)
                gl.DeleteSync(composition_fence);
            for (auto& player : players) {
                if (player.render_fence)
                    gl.DeleteSync(player.render_fence);
                if (player.read_fbo)
                    gl.DeleteFramebuffers(1, &player.read_fbo);
                if (player.texture)
                    gl.DeleteTextures(1, &player.texture);
            }
        }
        // FBOs in the player contexts are released when their contexts are destroyed.
        for (const auto& player : players) {
            if (player.context)
                contexts.destroy(player.context);
        }
    }
};

GpuCompositor::GpuCompositor() = default;
GpuCompositor::~GpuCompositor() = default;

void GpuCompositor::initialize(retro_hw_get_proc_address_t resolver, unsigned width,
                               unsigned height, unsigned players) {
    auto candidate = std::make_unique<State>(resolver, width, height, players);
    candidate->setup();
    state = std::move(candidate);
}

void GpuCompositor::reset() { state.reset(); }
bool GpuCompositor::ready() const { return state != nullptr; }
void GpuCompositor::activate(unsigned player) { state->activate(state->players[player].context); }
void GpuCompositor::activate_frontend() { state->activate(state->contexts.frontend); }
uintptr_t GpuCompositor::player_framebuffer(unsigned player) const {
    return state ? state->players[player].render_fbo : 0;
}

void GpuCompositor::start_player_frame() {
    if (state->composition_fence)
        state->gl.WaitSync(state->composition_fence, 0, UINT64_MAX);
}

void GpuCompositor::finish_player_frame(unsigned player) {
    auto& target = state->players[player];
    if (target.render_fence)
        state->gl.DeleteSync(target.render_fence);
    target.render_fence = state->gl.FenceSync(sync_gpu_commands_complete, 0);
    state->gl.Flush();
}

void GpuCompositor::compose(retro_hw_get_current_framebuffer_t frontend_framebuffer) {
    auto& gl = state->gl;
    int old_read{}, old_draw{};
    gl.GetIntegerv(read_framebuffer_binding, &old_read);
    gl.GetIntegerv(draw_framebuffer_binding, &old_draw);
    const bool scissor = gl.IsEnabled(scissor_test) != 0;
    if (scissor)
        gl.Disable(scissor_test);
    gl.BindFramebuffer(draw_framebuffer, static_cast<Name>(frontend_framebuffer()));
    for (unsigned i = 0; i < state->count; ++i) {
        const auto& player = state->players[i];
        if (player.render_fence)
            gl.WaitSync(player.render_fence, 0, UINT64_MAX);
        const int x = static_cast<int>(i) * state->width;
        gl.BindFramebuffer(read_framebuffer, player.read_fbo);
        gl.BlitFramebuffer(0, 0, state->width, state->height, x, 0, x + state->width,
                           state->height, color_buffer_bit, nearest);
    }
    if (state->composition_fence)
        gl.DeleteSync(state->composition_fence);
    state->composition_fence = gl.FenceSync(sync_gpu_commands_complete, 0);
    gl.Flush();
    gl.BindFramebuffer(read_framebuffer, old_read);
    gl.BindFramebuffer(draw_framebuffer, old_draw);
    if (scissor)
        gl.Enable(scissor_test);
}

void GpuCompositor::draw_menu(retro_hw_get_current_framebuffer_t frontend_framebuffer,
                              const uint32_t* pixels, unsigned selection) {
    constexpr Enum texture_binding_2d = 0x8069;
    constexpr Enum pixel_unpack_buffer = 0x88EC;
    constexpr Enum pixel_unpack_buffer_binding = 0x88EF;
    constexpr Enum unpack_row_length = 0x0CF2;
    constexpr Enum bgra = 0x80E1;
    constexpr Enum unsigned_byte = 0x1401;
    auto& gl = state->gl;
    const int width = state->width;
    const int height = state->height;
    int old_read{}, old_draw{};
    gl.GetIntegerv(read_framebuffer_binding, &old_read);
    gl.GetIntegerv(draw_framebuffer_binding, &old_draw);
    if (state->menu_selection != selection) {
        int bound_texture{}, bound_buffer{}, row_length{};
        gl.GetIntegerv(texture_binding_2d, &bound_texture);
        gl.GetIntegerv(pixel_unpack_buffer_binding, &bound_buffer);
        gl.GetIntegerv(unpack_row_length, &row_length);
        if (!state->menu_texture) {
            gl.GenTextures(1, &state->menu_texture);
            gl.BindTexture(texture_2d, state->menu_texture);
            gl.TexStorage2D(texture_2d, 1, rgba8, width, height);
            gl.GenFramebuffers(1, &state->menu_fbo);
            attach_color(gl, read_framebuffer, state->menu_fbo, state->menu_texture);
        } else {
            gl.BindTexture(texture_2d, state->menu_texture);
        }
        gl.BindBuffer(pixel_unpack_buffer, 0);
        gl.PixelStorei(unpack_row_length, 0);
        gl.TexSubImage2D(texture_2d, 0, 0, 0, width, height, bgra, unsigned_byte, pixels);
        gl.PixelStorei(unpack_row_length, row_length);
        gl.BindBuffer(pixel_unpack_buffer, bound_buffer);
        gl.BindTexture(texture_2d, bound_texture);
        state->menu_selection = selection;
    }
    const bool scissor = gl.IsEnabled(scissor_test) != 0;
    if (scissor)
        gl.Disable(scissor_test);
    gl.BindFramebuffer(draw_framebuffer, static_cast<Name>(frontend_framebuffer()));
    gl.BindFramebuffer(read_framebuffer, state->menu_fbo);
    gl.BlitFramebuffer(0, 0, width, height, 0, height, width, 0, color_buffer_bit, nearest);
    gl.BindFramebuffer(read_framebuffer, old_read);
    gl.BindFramebuffer(draw_framebuffer, old_draw);
    if (scissor)
        gl.Enable(scissor_test);
}

#ifdef _WIN32
namespace {
struct ShaderWorkerContext {
    HWND window{};
    HDC dc{};
    HGLRC gl{};
    ~ShaderWorkerContext() {
        if (gl)
            wglDeleteContext(gl);
        if (dc)
            ReleaseDC(window, dc);
        if (window)
            DestroyWindow(window);
    }
};
}

void* create_shader_worker_context() {
    const HDC current_dc = wglGetCurrentDC();
    const HGLRC share = wglGetCurrentContext();
    if (!current_dc || !share)
        return nullptr;
    const int pixel_format = GetPixelFormat(current_dc);
    PIXELFORMATDESCRIPTOR descriptor{};
    if (pixel_format == 0 ||
        DescribePixelFormat(current_dc, pixel_format, sizeof(descriptor), &descriptor) == 0)
        return nullptr;

    auto worker = std::make_unique<ShaderWorkerContext>();
    worker->window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!worker->window)
        return nullptr;
    worker->dc = GetDC(worker->window);
    if (!worker->dc || !SetPixelFormat(worker->dc, pixel_format, &descriptor))
        return nullptr;
    try {
        NativeContexts context{worker->dc, share};
        worker->gl = context.create();
    } catch (const std::exception&) {
        return nullptr;
    }
    return worker->gl ? worker.release() : nullptr;
}

bool make_shader_worker_current(void* context) {
    auto& worker = *static_cast<ShaderWorkerContext*>(context);
    return wglMakeCurrent(worker.dc, worker.gl) != 0;
}

void clear_shader_worker_current(void*) { wglMakeCurrent(nullptr, nullptr); }

#else
extern "C" void* eglCreatePbufferSurface(void*, void*, const int*);
extern "C" unsigned eglDestroySurface(void*, void*);

namespace {
struct ShaderWorkerContext {
    void* display{};
    void* surface{};
    void* gl{};
    ~ShaderWorkerContext() {
        if (gl)
            eglDestroyContext(display, gl);
        if (surface)
            eglDestroySurface(display, surface);
    }
};
}

void* create_shader_worker_context() {
    const auto display = eglGetCurrentDisplay();
    const auto share = eglGetCurrentContext();
    int config_id{}, count{};
    void* config{};
    if (!display || !share || !eglQueryContext(display, share, 0x3028, &config_id))
        return nullptr;
    const int config_attributes[] = {0x3028, config_id, 0x3038};
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count == 0)
        return nullptr;

    auto worker = std::make_unique<ShaderWorkerContext>();
    worker->display = display;
    const int surface_attributes[] = {0x3057, 1, 0x3056, 1, 0x3038};
    worker->surface = eglCreatePbufferSurface(display, config, surface_attributes);
    const int attributes[] = {0x3098, 4, 0x30FB, 3, 0x30FD, 1, 0x3038};
    worker->gl = eglCreateContext(display, config, share, attributes);
    return worker->gl && worker->surface ? worker.release() : nullptr;
}

bool make_shader_worker_current(void* context) {
    auto& worker = *static_cast<ShaderWorkerContext*>(context);
    return eglBindAPI(0x30A2) &&
           eglMakeCurrent(worker.display, worker.surface, worker.surface, worker.gl);
}

void clear_shader_worker_current(void* context) {
    auto& worker = *static_cast<ShaderWorkerContext*>(context);
    eglMakeCurrent(worker.display, nullptr, nullptr, nullptr);
}
#endif

void destroy_shader_worker_context(void* context) {
    delete static_cast<ShaderWorkerContext*>(context);
}

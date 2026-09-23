/*
 * Mickey's Wild Adventure — integrated host frontend.
 *
 * Presentation-only: it never touches PS1 RAM/VRAM or generated game code.
 * It runs inside the same SDL/OpenGL window as the recomp, before emulation,
 * then leaves the existing psxrecomp renderer in control.
 * MICKEY_RA_I18N_V1_2_2_BADGES_TOAST
 * MICKEY_RA_I18N_V1_3_2_BADGE_EVENT_FIX
 * MICKEY_INPUT_REBIND_DYNAMIC_LEGENDS_V1
 */
#include "mickey_frontend.h"
#include "mickey_profiles.h"
#include "starvation_ring.h"
#include "mickey_ra.h"
#ifndef _WIN32
#include "mickey_http.h"
#endif
#include "psx_sdl.h"
#include "psx_sdl_audio.h"
#include "gpu_gl_renderer.h"
#include "mod_plugins.h"
#include "psx_keybinds.h"
#include "mickey_input_bridge.h"

#define STBI_NO_STDIO
#include "../third_party/stb_image.h"

#if defined(__ANDROID__)
#include <GLES3/gl3.h>
#elif defined(PSX_SDL3)
#include <SDL3/SDL_opengl.h>
#else
#include <SDL_opengl.h>
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

/* MICKEY_RA_I18N_V1_3_1_SFX_SCOPE_FIX */
static void mickey_ui_play_sfx(const wchar_t *filename);

namespace {

constexpr float VW = 1920.0f;
constexpr float VH = 1080.0f;

SDL_Window *g_window = nullptr;
int g_drawable_w = 1;
int g_drawable_h = 1;
bool g_initialized = false;
bool g_ran = false;
bool g_game_open_active = false;
uint64_t g_game_open_start = 0;

/* MICKEY_NEWGAME_TITLE_INPUT_GUARD_V1
 * The same Enter/A press used to choose a NEW GAME profile was still visible
 * to the PS1 immediately after the host frontend returned. The original
 * title screen consumed that held press and instantly advanced to its menu.
 * CONTINUE is intentionally unaffected.
 */
bool g_mickey_newgame_title_input_guard = false;
int  g_mickey_newgame_title_neutral_frames = 0;
bool g_gameover_deferred_load_black_hold = false; /* MICKEY_GAMEOVER_V43_PRESENT_SAFELOAD */

enum class PendingUiAction {
    None, Resume, ApplyDisplaySettings, ReturnToFrontend, RestartGame, Quit
};
enum class UiModalOwner { None, Pause, GameOver };
UiModalOwner g_ui_modal_owner = UiModalOwner::None;
PendingUiAction g_pending_ui_action = PendingUiAction::None;
bool g_pending_display_window_change = false;

struct UiModalGuard {
    UiModalOwner owner;
    bool acquired;
    explicit UiModalGuard(UiModalOwner value)
        : owner(value), acquired(g_ui_modal_owner == UiModalOwner::None) {
        if (acquired) g_ui_modal_owner = owner;
    }
    ~UiModalGuard() {
        if (acquired) g_ui_modal_owner = UiModalOwner::None;
    }
};

struct Config {
    int bezel = 1;       // 0 none, 1..4 artwork
    int resolution = 2;  // 0 720p, 1 900p, 2 1080p, 3 desktop
    int window_mode = 1; // 0 windowed, 1 borderless, 2 exclusive fullscreen
    int crt_mode = 0;    // 0 off, 1 PVM, 2 classic, 3 consumer TV, 4 arcade, 5 strong
    int language = 0;    // 0 PL, 1 EN, 2 IT, 3 JA, 4 DE
} g_cfg;

/* ---- Frontend audio -----------------------------------------------------
 * Menu audio owns a temporary SDL playback device only while the frontend is
 * visible. It is closed before the guest starts so psxrecomp can open its
 * normal SPU device with no contention.
 */
struct PcmClip {
    std::vector<int16_t> pcm; /* interleaved stereo, signed 16-bit, 44100 Hz */
    size_t frames = 0;
};

PcmClip g_menu_music;
PcmClip g_title_music;
PcmClip g_sfx_pickup;
PcmClip g_sfx_enter;

/* MICKEY_STARTUP_AUDIO_SPLIT_V1_2 */
bool g_menu_music_enabled = true;
bool g_title_music_active = false;
size_t g_title_music_pos = 0;

SDL_AudioDeviceID g_menu_audio_device = 0;
#if defined(PSX_SDL3)
SDL_AudioStream *g_menu_audio_stream = nullptr;
#endif
size_t g_music_pos = 0;
std::atomic<long long> g_pickup_pos{-1};
std::atomic<long long> g_enter_pos{-1};

constexpr int MENU_AUDIO_HZ = 44100;
constexpr int MENU_AUDIO_CHANNELS = 2;
constexpr float MENU_MUSIC_GAIN = 0.28f;
constexpr float TITLE_MUSIC_GAIN = 0.58f;
constexpr float MENU_SFX_GAIN = 0.88f;
constexpr size_t MENU_LOOP_XFADE_FRAMES = 22050; /* 0.5 s */

#if defined(PSX_SDL3)
constexpr SDL_AudioFormat MENU_AUDIO_FORMAT = SDL_AUDIO_S16;
#else
constexpr SDL_AudioFormat MENU_AUDIO_FORMAT = AUDIO_S16SYS;
#endif

/* ---- OpenGL dynamic API ------------------------------------------------ */
using FnCreateShader = GLuint (*)(GLenum);
using FnShaderSource = void (*)(GLuint, GLsizei, const GLchar *const *, const GLint *);
using FnCompileShader = void (*)(GLuint);
using FnGetShaderiv = void (*)(GLuint, GLenum, GLint *);
using FnGetShaderInfoLog = void (*)(GLuint, GLsizei, GLsizei *, GLchar *);
using FnDeleteShader = void (*)(GLuint);
using FnCreateProgram = GLuint (*)();
using FnAttachShader = void (*)(GLuint, GLuint);
using FnLinkProgram = void (*)(GLuint);
using FnGetProgramiv = void (*)(GLuint, GLenum, GLint *);
using FnGetProgramInfoLog = void (*)(GLuint, GLsizei, GLsizei *, GLchar *);
using FnDeleteProgram = void (*)(GLuint);
using FnUseProgram = void (*)(GLuint);
using FnGetUniformLocation = GLint (*)(GLuint, const GLchar *);
using FnUniform1i = void (*)(GLint, GLint);
using FnUniform1f = void (*)(GLint, GLfloat);
using FnUniform2f = void (*)(GLint, GLfloat, GLfloat);
using FnUniform4f = void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using FnGenVertexArrays = void (*)(GLsizei, GLuint *);
using FnBindVertexArray = void (*)(GLuint);
using FnDeleteVertexArrays = void (*)(GLsizei, const GLuint *);
using FnActiveTexture = void (*)(GLenum);

FnCreateShader pCreateShader = nullptr;
FnShaderSource pShaderSource = nullptr;
FnCompileShader pCompileShader = nullptr;
FnGetShaderiv pGetShaderiv = nullptr;
FnGetShaderInfoLog pGetShaderInfoLog = nullptr;
FnDeleteShader pDeleteShader = nullptr;
FnCreateProgram pCreateProgram = nullptr;
FnAttachShader pAttachShader = nullptr;
FnLinkProgram pLinkProgram = nullptr;
FnGetProgramiv pGetProgramiv = nullptr;
FnGetProgramInfoLog pGetProgramInfoLog = nullptr;
FnDeleteProgram pDeleteProgram = nullptr;
FnUseProgram pUseProgram = nullptr;
FnGetUniformLocation pGetUniformLocation = nullptr;
FnUniform1i pUniform1i = nullptr;
FnUniform1f pUniform1f = nullptr;
FnUniform2f pUniform2f = nullptr;
FnUniform4f pUniform4f = nullptr;
FnGenVertexArrays pGenVertexArrays = nullptr;
FnBindVertexArray pBindVertexArray = nullptr;
FnDeleteVertexArrays pDeleteVertexArrays = nullptr;
FnActiveTexture pActiveTexture = nullptr;

template <typename T>
bool load_gl(T &dst, const char *name) {
    dst = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (!dst) {
        std::fprintf(stderr, "mickey_frontend: missing GL function %s\n", name);
        return false;
    }
    return true;
}

GLuint g_prog = 0;
GLuint g_vao = 0;
GLint u_mode=-1, u_rect=-1, u_tint=-1, u_tex=-1, u_circle_center=-1,
      u_circle_radius=-1, u_aspect=-1, u_crt_preview=-1,
      u_crt_strength=-1, u_crt_gamma=-1, u_crt_lines=-1;

struct Tex {
    GLuint id=0;
    int w=0;
    int h=0;
};

Tex bg_tex, logo_tex, preview_test_tex;
Tex main_tex[4];
Tex opt_label_tex[5];
Tex bezel_value_tex[5];
Tex res_value_tex[4];
Tex mode_value_tex[3];
Tex crt_value_tex[6];
Tex page_title_tex, preview_title_tex, hint_lr_tex;
Tex bezel_preview_tex[5];
Tex glyph_tex[128];
std::unordered_map<uint32_t,Tex> glyph_unicode;
Tex profile_thumb_tex[3];
Tex profile_title_new_tex, profile_title_continue_tex, profile_hint_tex, saving_tex;

/* MICKEY_STARTUP_SEQUENCE_V1 */
bool g_mickey_boot_intro_seen=false;

std::unordered_map<std::string,fs::path> g_ra_badge_manifest;
std::unordered_map<std::string,Tex> g_ra_badge_cache;
std::unordered_map<std::string,bool> g_ra_badge_attempted;
bool g_ra_badges_manifest_loaded=false;
uint64_t g_ra_badge_last_download_tick=0;
std::unordered_map<std::string,int> g_ra_unlock_known;
bool g_ra_unlock_seeded=false;
struct RAUnlockToast { bool active=false; uint64_t start=0; uint32_t id=0; std::string title; std::string desc; std::string badge_url; bool unlocked=false; };
std::vector<RAUnlockToast> g_ra_toast_queue;
RAUnlockToast g_ra_toast_current;


const char *VS = R"GLSL(
#version 330
out vec2 v_screen;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_screen = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char *FS = R"GLSL(
#version 330
in vec2 v_screen;
out vec4 frag;
uniform sampler2D u_tex;
uniform int u_mode;
uniform vec4 u_rect;
uniform vec4 u_tint;
uniform vec2 u_circle_center;
uniform float u_circle_radius;
uniform float u_aspect;
uniform int u_crt_preview;
uniform float u_crt_strength;
uniform float u_crt_gamma;
uniform float u_crt_lines;

void main() {
    if (u_mode == 2) {
        vec2 d = v_screen - u_circle_center;
        d.x *= u_aspect;
        if (length(d) <= u_circle_radius) discard;
        frag = u_tint;
        return;
    }

    vec2 q = (v_screen - u_rect.xy) / u_rect.zw;
    if (q.x < 0.0 || q.x > 1.0 || q.y < 0.0 || q.y > 1.0)
        discard;

    if (u_mode == 1) {
        frag = u_tint;
        return;
    }

    vec2 uv = vec2(q.x, 1.0 - q.y);
    vec4 c = texture(u_tex, uv) * u_tint;

    if (u_crt_preview != 0) {
        float p = fract(q.y * max(u_crt_lines, 1.0));
        float beam = sin(3.14159265 * p);
        float mult = 1.0 - u_crt_strength * (1.0 - beam);
        c.rgb *= mult;
        if (u_crt_gamma > 0.0 && abs(u_crt_gamma - 1.0) > 0.001)
            c.rgb = pow(max(c.rgb, vec3(0.0)), vec3(1.0 / u_crt_gamma));
    }

    if (c.a <= 0.002) discard;
    frag = c;
}
)GLSL";

GLuint compile_shader(GLenum type, const char *src) {
#if defined(__ANDROID__)
    std::string es_source(src);
    const std::string desktop_version="#version 330";
    const size_t version_pos=es_source.find_first_not_of(" \t\r\n");
    if(version_pos!=std::string::npos &&
       es_source.compare(version_pos,desktop_version.size(),desktop_version)==0) {
        const size_t line_end=es_source.find('\n',version_pos);
        const size_t version_end=(line_end==std::string::npos)
            ? es_source.size() : line_end+1;
        es_source.replace(0,version_end,
            "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n");
    }
    src=es_source.c_str();
#endif
    GLuint sh = pCreateShader(type);
    pShaderSource(sh, 1, &src, nullptr);
    pCompileShader(sh);
    GLint ok=0;
    pGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]={0};
        pGetShaderInfoLog(sh, sizeof(log)-1, nullptr, log);
        std::fprintf(stderr, "mickey_frontend shader: %s\n", log);
        pDeleteShader(sh);
        return 0;
    }
    return sh;
}

bool init_gl() {
    bool ok = true;
    ok &= load_gl(pCreateShader,"glCreateShader");
    ok &= load_gl(pShaderSource,"glShaderSource");
    ok &= load_gl(pCompileShader,"glCompileShader");
    ok &= load_gl(pGetShaderiv,"glGetShaderiv");
    ok &= load_gl(pGetShaderInfoLog,"glGetShaderInfoLog");
    ok &= load_gl(pDeleteShader,"glDeleteShader");
    ok &= load_gl(pCreateProgram,"glCreateProgram");
    ok &= load_gl(pAttachShader,"glAttachShader");
    ok &= load_gl(pLinkProgram,"glLinkProgram");
    ok &= load_gl(pGetProgramiv,"glGetProgramiv");
    ok &= load_gl(pGetProgramInfoLog,"glGetProgramInfoLog");
    ok &= load_gl(pDeleteProgram,"glDeleteProgram");
    ok &= load_gl(pUseProgram,"glUseProgram");
    ok &= load_gl(pGetUniformLocation,"glGetUniformLocation");
    ok &= load_gl(pUniform1i,"glUniform1i");
    ok &= load_gl(pUniform1f,"glUniform1f");
    ok &= load_gl(pUniform2f,"glUniform2f");
    ok &= load_gl(pUniform4f,"glUniform4f");
    ok &= load_gl(pGenVertexArrays,"glGenVertexArrays");
    ok &= load_gl(pBindVertexArray,"glBindVertexArray");
    ok &= load_gl(pDeleteVertexArrays,"glDeleteVertexArrays");
    ok &= load_gl(pActiveTexture,"glActiveTexture");
    if (!ok) return false;

    GLuint vs=compile_shader(GL_VERTEX_SHADER,VS);
    GLuint fs=compile_shader(GL_FRAGMENT_SHADER,FS);
    if (!vs || !fs) return false;
    g_prog=pCreateProgram();
    pAttachShader(g_prog,vs);
    pAttachShader(g_prog,fs);
    pLinkProgram(g_prog);
    pDeleteShader(vs);
    pDeleteShader(fs);
    GLint linked=0;
    pGetProgramiv(g_prog,GL_LINK_STATUS,&linked);
    if (!linked) {
        char log[2048]={0};
        pGetProgramInfoLog(g_prog,sizeof(log)-1,nullptr,log);
        std::fprintf(stderr,"mickey_frontend program: %s\n",log);
        return false;
    }
    u_mode=pGetUniformLocation(g_prog,"u_mode");
    u_rect=pGetUniformLocation(g_prog,"u_rect");
    u_tint=pGetUniformLocation(g_prog,"u_tint");
    u_tex=pGetUniformLocation(g_prog,"u_tex");
    u_circle_center=pGetUniformLocation(g_prog,"u_circle_center");
    u_circle_radius=pGetUniformLocation(g_prog,"u_circle_radius");
    u_aspect=pGetUniformLocation(g_prog,"u_aspect");
    u_crt_preview=pGetUniformLocation(g_prog,"u_crt_preview");
    u_crt_strength=pGetUniformLocation(g_prog,"u_crt_strength");
    u_crt_gamma=pGetUniformLocation(g_prog,"u_crt_gamma");
    u_crt_lines=pGetUniformLocation(g_prog,"u_crt_lines");
    pGenVertexArrays(1,&g_vao);
    return true;
}

/* ---- Paths / config ----------------------------------------------------- */
fs::path exe_dir() {
#if defined(__ANDROID__)
    const char *app_data=SDL_GetAndroidInternalStoragePath();
    if(app_data && app_data[0]) return fs::path(app_data);
#endif
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n=GetModuleFileNameW(nullptr,buf,(DWORD)(sizeof(buf)/sizeof(buf[0])));
    if (n>0 && n<(DWORD)(sizeof(buf)/sizeof(buf[0])))
        return fs::path(std::wstring(buf,buf+n)).parent_path();
#endif
    const char *base=SDL_GetBasePath();
    if (base && base[0]) {
        fs::path p(base);
#if !defined(PSX_SDL3)
        SDL_free((void*)base);
#endif
        return p;
    }
    return fs::current_path();
}

fs::path find_asset(const fs::path &rel) {
    const fs::path e=exe_dir();
    const fs::path c=fs::current_path();
    const fs::path candidates[] = {
        e / "assets" / "frontend" / rel,
        c / "MickeyFrontend" / "assets" / "frontend" / rel,
        c / "assets" / "frontend" / rel,
        e / ".." / "MickeyFrontend" / "assets" / "frontend" / rel,
    };
    for (const auto &p: candidates) {
        std::error_code ec;
        if (fs::is_regular_file(p,ec)) return p;
    }
    return {};
}


static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool load_pcm16_stereo_wav(const fs::path &path, PcmClip &clip) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize sz=f.tellg();
    if (sz < 44) return false;
    std::vector<unsigned char> bytes((size_t)sz);
    f.seekg(0);
    if (!f.read((char*)bytes.data(),sz)) return false;

    if (std::memcmp(bytes.data(),"RIFF",4)!=0 ||
        std::memcmp(bytes.data()+8,"WAVE",4)!=0)
        return false;

    int fmt_tag=0, channels=0, rate=0, bits=0;
    const unsigned char *data=nullptr;
    size_t data_bytes=0;

    size_t off=12;
    while (off+8<=bytes.size()) {
        const unsigned char *h=bytes.data()+off;
        const uint32_t n=rd32(h+4);
        const size_t body=off+8;
        if (body+n>bytes.size()) break;

        if (std::memcmp(h,"fmt ",4)==0 && n>=16) {
            fmt_tag=(int)rd16(bytes.data()+body+0);
            channels=(int)rd16(bytes.data()+body+2);
            rate=(int)rd32(bytes.data()+body+4);
            bits=(int)rd16(bytes.data()+body+14);
        } else if (std::memcmp(h,"data",4)==0) {
            data=bytes.data()+body;
            data_bytes=n;
        }

        off=body+n+(n&1u);
    }

    if (!data || fmt_tag!=1 || channels!=2 ||
        rate!=MENU_AUDIO_HZ || bits!=16 || data_bytes<4)
        return false;

    const size_t samples=data_bytes/2;
    clip.pcm.resize(samples);
    std::memcpy(clip.pcm.data(),data,samples*sizeof(int16_t));
    clip.frames=samples/2;
    return clip.frames>0;
}

static inline int16_t clamp16(int x) {
    if (x>32767) return 32767;
    if (x<-32768) return -32768;
    return (int16_t)x;
}

static void mix_sfx_sample(const PcmClip &clip,
                           std::atomic<long long> &play_pos,
                           int &l, int &r) {
    long long pos=play_pos.load(std::memory_order_relaxed);
    if (pos<0) return;
    if ((size_t)pos>=clip.frames) {
        play_pos.store(-1,std::memory_order_relaxed);
        return;
    }

    const size_t i=(size_t)pos*2;
    l += (int)((float)clip.pcm[i+0]*MENU_SFX_GAIN);
    r += (int)((float)clip.pcm[i+1]*MENU_SFX_GAIN);

    ++pos;
    play_pos.store((size_t)pos>=clip.frames?-1:pos,
                   std::memory_order_relaxed);
}

void SDLCALL menu_audio_callback(void *, Uint8 *stream, int len) {
    if (!stream || len<=0) return;
    std::memset(stream,0,(size_t)len);

    int16_t *out=(int16_t*)stream;
    const int frames=len/4;
    if (frames<=0) return;

    for (int f=0; f<frames; ++f) {
        int l=0, r=0;

        if (g_title_music_active && g_title_music.frames>0) {
            if (g_title_music_pos < g_title_music.frames) {
                const size_t i=g_title_music_pos*2;
                l+=(int)((float)g_title_music.pcm[i+0]*TITLE_MUSIC_GAIN);
                r+=(int)((float)g_title_music.pcm[i+1]*TITLE_MUSIC_GAIN);
                ++g_title_music_pos;
            }
            if (g_title_music_pos>=g_title_music.frames)
                g_title_music_active=false; /* ONE-SHOT: title fanfare NIE loopuje */
        } else if (g_menu_music_enabled && g_menu_music.frames>0) {
            if (g_music_pos>=g_menu_music.frames)
                g_music_pos=std::min(MENU_LOOP_XFADE_FRAMES,
                                     g_menu_music.frames-1);

            const size_t pos=g_music_pos;
            const size_t i=pos*2;
            float ml=(float)g_menu_music.pcm[i+0];
            float mr=(float)g_menu_music.pcm[i+1];

            /* MENU zostaje zloopowane tak jak wcześniej. */
            const size_t xf=std::min(MENU_LOOP_XFADE_FRAMES,
                                     g_menu_music.frames/4);
            if (xf>0 && pos>=g_menu_music.frames-xf) {
                const size_t k=pos-(g_menu_music.frames-xf);
                const float t=(xf>1)?(float)k/(float)(xf-1):1.0f;
                const size_t hi=std::min(k,g_menu_music.frames-1)*2;
                const float a=std::cos(t*1.57079632679f);
                const float b=std::sin(t*1.57079632679f);
                ml=ml*a+(float)g_menu_music.pcm[hi+0]*b;
                mr=mr*a+(float)g_menu_music.pcm[hi+1]*b;
            }

            l+=(int)(ml*MENU_MUSIC_GAIN);
            r+=(int)(mr*MENU_MUSIC_GAIN);

            ++g_music_pos;
            if (g_music_pos>=g_menu_music.frames)
                g_music_pos=std::min(xf,g_menu_music.frames-1);
        }

        mix_sfx_sample(g_sfx_pickup,g_pickup_pos,l,r);
        mix_sfx_sample(g_sfx_enter,g_enter_pos,l,r);

        out[f*2+0]=clamp16(l);
        out[f*2+1]=clamp16(r);
    }
}

void menu_audio_pump() {
#if defined(PSX_SDL3)
    if (!g_menu_audio_stream) return;
#else
    if (!g_menu_audio_device) return;
#endif

    constexpr Uint32 CHUNK_FRAMES = 1024;
    constexpr Uint32 CHUNK_BYTES  = CHUNK_FRAMES * 2u * (Uint32)sizeof(int16_t);
    constexpr Uint32 TARGET_BYTES = CHUNK_BYTES * 3u;

    int guard = 0;
    while (guard++ < 5) {
#if defined(PSX_SDL3)
        const int q = SDL_GetAudioStreamQueued(g_menu_audio_stream);
        const Uint32 queued = q > 0 ? (Uint32)q : 0u;
#else
        const Uint32 queued = psx_sdl_audio_queued_size(g_menu_audio_device);
#endif
        if (queued >= TARGET_BYTES) break;

        int16_t buffer[CHUNK_FRAMES * 2];
        menu_audio_callback(nullptr,
                            reinterpret_cast<Uint8*>(buffer),
                            (int)sizeof(buffer));

#if defined(PSX_SDL3)
        if (!SDL_PutAudioStreamData(g_menu_audio_stream,
                                    buffer,
                                    (int)sizeof(buffer))) {
            static bool once=false;
            if (!once) {
                once=true;
                std::fprintf(stderr,
                    "mickey_frontend: SDL3 PutAudioStreamData failed: %s\n",
                    SDL_GetError());
            }
            break;
        }
#else
        if (psx_sdl_audio_queue(g_menu_audio_device,
                                buffer,
                                (Uint32)sizeof(buffer)) != 0) {
            break;
        }
#endif
    }
}

void menu_audio_pickup() {
    if (g_menu_audio_device && g_sfx_pickup.frames)
        g_pickup_pos.store(0,std::memory_order_relaxed);
}

void menu_audio_enter() {
    if (g_menu_audio_device && g_sfx_enter.frames)
        g_enter_pos.store(0,std::memory_order_relaxed);
}

void menu_audio_start() {
#if defined(PSX_SDL3)
    if (g_menu_audio_stream) return;
#else
    if (g_menu_audio_device) return;
#endif

    const fs::path music=find_asset("audio/menu_music.wav");
    const fs::path title=find_asset("audio/title_music.wav");
    const fs::path pickup=find_asset("audio/pickup.wav");
    const fs::path enter=find_asset("audio/enter.wav");

    const bool ok_music=load_pcm16_stereo_wav(music,g_menu_music);
    const bool ok_title=load_pcm16_stereo_wav(title,g_title_music);
    const bool ok_pickup=load_pcm16_stereo_wav(pickup,g_sfx_pickup);
    const bool ok_enter=load_pcm16_stereo_wav(enter,g_sfx_enter);

    std::ofstream dbg(exe_dir()/"frontend_audio_debug.txt",
                      std::ios::out|std::ios::trunc);
    if (dbg) {
        dbg << "Mickey Frontend Audio v3.3\n";
        dbg << "music_path=" << music.string() << " frames=" << g_menu_music.frames
            << " ok=" << ok_music << "\n";
        dbg << "title_path=" << title.string() << " frames=" << g_title_music.frames
            << " ok=" << ok_title << "\n";
        dbg << "pickup_path=" << pickup.string() << " frames=" << g_sfx_pickup.frames
            << " ok=" << ok_pickup << "\n";
        dbg << "enter_path=" << enter.string() << " frames=" << g_sfx_enter.frames
            << " ok=" << ok_enter << "\n";
    }

    if (!ok_music)
        std::fprintf(stderr,"mickey_frontend: menu_music.wav missing/invalid\n");
    if (!ok_title)
        std::fprintf(stderr,"mickey_frontend: title_music.wav missing/invalid\n");
    if (!ok_pickup)
        std::fprintf(stderr,"mickey_frontend: pickup.wav missing/invalid\n");
    if (!ok_enter)
        std::fprintf(stderr,"mickey_frontend: enter.wav missing/invalid\n");

    if (g_menu_music.frames==0 &&
        g_title_music.frames==0 &&
        g_sfx_pickup.frames==0 &&
        g_sfx_enter.frames==0) {
        if (dbg) dbg << "RESULT=no_audio_files_loaded\n";
        return;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO)!=0) {
        const char *err=SDL_GetError();
        std::fprintf(stderr,"mickey_frontend: SDL audio init failed: %s\n",err);
        if (dbg) dbg << "RESULT=SDL_InitSubSystem_failed error=" << err << "\n";
        return;
    }

    g_music_pos=0;
    g_title_music_pos=0;
    g_pickup_pos.store(-1,std::memory_order_relaxed);
    g_enter_pos.store(-1,std::memory_order_relaxed);

#if defined(PSX_SDL3)
    SDL_AudioSpec spec{};
    spec.freq=MENU_AUDIO_HZ;
    spec.format=MENU_AUDIO_FORMAT;
    spec.channels=MENU_AUDIO_CHANNELS;

    g_menu_audio_stream=SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec,
        nullptr,
        nullptr);

    if (!g_menu_audio_stream) {
        const char *err=SDL_GetError();
        std::fprintf(stderr,
            "mickey_frontend: direct SDL3 menu stream failed: %s\n",err);
        if (dbg) dbg << "RESULT=SDL_OpenAudioDeviceStream_failed error=" << err << "\n";
        return;
    }

    g_menu_audio_device=SDL_GetAudioStreamDevice(g_menu_audio_stream);
    if (!g_menu_audio_device) {
        const char *err=SDL_GetError();
        std::fprintf(stderr,
            "mickey_frontend: SDL_GetAudioStreamDevice failed: %s\n",err);
        if (dbg) dbg << "RESULT=GetAudioStreamDevice_failed error=" << err << "\n";
        SDL_DestroyAudioStream(g_menu_audio_stream);
        g_menu_audio_stream=nullptr;
        return;
    }

    if (!SDL_ResumeAudioStreamDevice(g_menu_audio_stream)) {
        const char *err=SDL_GetError();
        std::fprintf(stderr,
            "mickey_frontend: SDL_ResumeAudioStreamDevice failed: %s\n",err);
        if (dbg) dbg << "RESULT=ResumeAudioStreamDevice_failed error=" << err << "\n";
        SDL_DestroyAudioStream(g_menu_audio_stream);
        g_menu_audio_stream=nullptr;
        g_menu_audio_device=0;
        return;
    }

    menu_audio_pump();
    const int queued=SDL_GetAudioStreamQueued(g_menu_audio_stream);
    if (dbg) {
        dbg << "device=" << (unsigned)g_menu_audio_device << "\n";
        dbg << "queued_after_prime=" << queued << "\n";
        dbg << "RESULT=OK_DIRECT_SDL3\n";
    }
    std::fprintf(stdout,
        "MICKEY_FRONTEND_AUDIO_V3_3: direct SDL3 stream, device=%u queued=%d\n",
        (unsigned)g_menu_audio_device, queued);
    std::fflush(stdout);
#else
    PsxSdlAudioSpec want{};
    PsxSdlAudioSpec have{};
    want.freq=MENU_AUDIO_HZ;
    want.format=MENU_AUDIO_FORMAT;
    want.channels=MENU_AUDIO_CHANNELS;
    want.samples=1024;
    want.allow_frequency_change=0;
    want.callback=nullptr;
    want.userdata=nullptr;

    g_menu_audio_device=psx_sdl_audio_open(&want,&have);
    if (!g_menu_audio_device) {
        if (dbg) dbg << "RESULT=SDL2_open_failed\n";
        return;
    }
    (void)psx_sdl_audio_resume(g_menu_audio_device);
    menu_audio_pump();
    if (dbg) dbg << "RESULT=OK_SDL2\n";
#endif
}

void menu_audio_stop() {
#if defined(PSX_SDL3)
    if (g_menu_audio_stream) {
        SDL_DestroyAudioStream(g_menu_audio_stream);
        g_menu_audio_stream=nullptr;
    }
    g_menu_audio_device=0;
#else
    if (g_menu_audio_device) {
        psx_sdl_audio_close(g_menu_audio_device);
        g_menu_audio_device=0;
    }
#endif
    g_pickup_pos.store(-1,std::memory_order_relaxed);
    g_enter_pos.store(-1,std::memory_order_relaxed);
    g_title_music_active=false;
    g_title_music_pos=0;
    g_menu_music_enabled=true;
}

fs::path cfg_path() { return exe_dir() / "mickey_frontend.ini"; }

void load_config() {
    std::ifstream f(cfg_path());
    if (!f) return;
    std::string line;
    while (std::getline(f,line)) {
        auto eq=line.find('=');
        if (eq==std::string::npos) continue;
        std::string k=line.substr(0,eq), v=line.substr(eq+1);
        int n=std::atoi(v.c_str());
        if (k=="bezel") g_cfg.bezel=std::clamp(n,0,4);
        else if (k=="resolution") g_cfg.resolution=std::clamp(n,0,3);
        else if (k=="window_mode") g_cfg.window_mode=std::clamp(n,0,2);
        else if (k=="crt_mode") g_cfg.crt_mode=std::clamp(n,0,5);
        else if (k=="language") g_cfg.language=std::clamp(n,0,4);
        else if (k=="crt") g_cfg.crt_mode=n?2:0; /* v1 compatibility */
    }
}

void save_config() {
    std::ofstream f(cfg_path(),std::ios::trunc);
    if (!f) return;
    f<<"bezel="<<g_cfg.bezel<<"\n";
    f<<"resolution="<<g_cfg.resolution<<"\n";
    f<<"window_mode="<<g_cfg.window_mode<<"\n";
    f<<"crt_mode="<<g_cfg.crt_mode<<"\n";
    f<<"language="<<g_cfg.language<<"\n";
}

#include "mickey_i18n.inc"

/* ---- Images ------------------------------------------------------------- */
bool read_file(const fs::path &p,std::vector<unsigned char> &out) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);
    if (!f) return false;
    auto n=f.tellg();
    if (n<=0) return false;
    out.resize((size_t)n);
    f.seekg(0);
    return !!f.read((char*)out.data(),n);
}

Tex load_tex(const fs::path &path) {
    Tex t;
    if (path.empty()) return t;
    std::vector<unsigned char> bytes;
    if (!read_file(path,bytes)) return t;
    int comp=0;
    unsigned char *px=stbi_load_from_memory(bytes.data(),(int)bytes.size(),&t.w,&t.h,&comp,4);
    if (!px) return t;
    glGenTextures(1,&t.id);
    glBindTexture(GL_TEXTURE_2D,t.id);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,t.w,t.h,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
    stbi_image_free(px);
    return t;
}

void load_assets() {
    bg_tex=load_tex(find_asset("background.png"));
    logo_tex=load_tex(find_asset("logo.png"));
    preview_test_tex=load_tex(find_asset("preview_test.png"));

    const char *m[4]={"labels/new_game.png","labels/continue.png","labels/settings.png","labels/exit.png"};
    for(int i=0;i<4;i++) main_tex[i]=load_tex(find_asset(m[i]));

    const char *ol[5]={"labels/bezel.png","labels/crt.png","labels/resolution.png","labels/window_mode.png","labels/back.png"};
    for(int i=0;i<5;i++) opt_label_tex[i]=load_tex(find_asset(ol[i]));

    const char *bv[5]={"labels/none.png","labels/b01.png","labels/b02.png","labels/b03.png","labels/b04.png"};
    for(int i=0;i<5;i++) bezel_value_tex[i]=load_tex(find_asset(bv[i]));

    const char *rv[4]={"labels/r720.png","labels/r900.png","labels/r1080.png","labels/rdesktop.png"};
    for(int i=0;i<4;i++) res_value_tex[i]=load_tex(find_asset(rv[i]));

    const char *wv[3]={"labels/windowed.png","labels/borderless.png","labels/fullscreen.png"};
    for(int i=0;i<3;i++) mode_value_tex[i]=load_tex(find_asset(wv[i]));

    const char *cv[6]={"labels/crt_off.png","labels/crt_pvm.png","labels/crt_classic.png",
                       "labels/crt_tv.png","labels/crt_arcade.png","labels/crt_strong.png"};
    for(int i=0;i<6;i++) crt_value_tex[i]=load_tex(find_asset(cv[i]));

    page_title_tex=load_tex(find_asset("labels/screen_settings.png"));
    preview_title_tex=load_tex(find_asset("labels/preview_live.png"));
    hint_lr_tex=load_tex(find_asset("labels/hint_lr.png"));
    profile_title_new_tex=load_tex(find_asset("labels/profile_new.png"));
    profile_title_continue_tex=load_tex(find_asset("labels/profile_continue.png"));
    profile_hint_tex=load_tex(find_asset("labels/profile_hint.png"));
    saving_tex=load_tex(find_asset("labels/saving.png"));
    for (int code=33; code<127; ++code) {
        char rel[40];
        std::snprintf(rel,sizeof(rel),"glyphs/%02X.png",code);
        glyph_tex[code]=load_tex(find_asset(rel));
    }
    {
        fs::path dir=exe_dir()/"assets"/"i18n"/"glyphs";
        std::error_code ec;
        if(!fs::is_directory(dir,ec)) dir=fs::current_path()/"MickeyFrontend"/"assets"/"i18n"/"glyphs";
        if(fs::is_directory(dir,ec)) for(const auto& e:fs::directory_iterator(dir,ec)) {
            if(ec || !e.is_regular_file()) continue;
            const std::string n=e.path().stem().string(); if(n.size()<2 || n[0]!='U') continue;
            char* end=nullptr; unsigned long cp=std::strtoul(n.c_str()+1,&end,16);
            if(end && *end==0 && cp>=128 && cp<=0x10FFFF) glyph_unicode[(uint32_t)cp]=load_tex(e.path());
        }
    }

    /* Preview uses the same Border/01..04 files as the game. */
    for(int i=1;i<=4;i++) {
        char name[16];
        std::snprintf(name,sizeof(name),"%02d.png",i);
        fs::path p=exe_dir()/"Border"/name;
        std::error_code ec;
        if (!fs::is_regular_file(p,ec))
            p=fs::current_path()/"Border"/name;
        if (fs::is_regular_file(p,ec))
            bezel_preview_tex[i]=load_tex(p);
    }
}

void gl_setup_frame(int *ww,int *wh) {
    SDL_GL_GetDrawableSize(g_window,ww,wh);
    if (*ww<1) *ww=1; if (*wh<1) *wh=1;
    g_drawable_w=*ww;
    g_drawable_h=*wh;
    glViewport(0,0,*ww,*wh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    pUseProgram(g_prog);
    pBindVertexArray(g_vao);
    pUniform1i(u_tex,0);
    pActiveTexture(GL_TEXTURE0);
}

void set_rect_virtual(float x,float y,float w,float h) {
    float nx=x/VW, nw=w/VW;
    float ny=1.0f-(y+h)/VH, nh=h/VH;
    pUniform4f(u_rect,nx,ny,nw,nh);

    const int x0=std::clamp((int)std::floor(x*(float)g_drawable_w/VW),0,g_drawable_w);
    const int x1=std::clamp((int)std::ceil((x+w)*(float)g_drawable_w/VW),0,g_drawable_w);
    const int y0=std::clamp((int)std::floor((VH-y-h)*(float)g_drawable_h/VH),0,g_drawable_h);
    const int y1=std::clamp((int)std::ceil((VH-y)*(float)g_drawable_h/VH),0,g_drawable_h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0,y0,std::max(0,x1-x0),std::max(0,y1-y0));
}

void frontend_present_idle(uint32_t milliseconds) {
#if defined(__ANDROID__)
    (void)milliseconds;
#else
    SDL_Delay(milliseconds);
#endif
}

void draw_solid(float x,float y,float w,float h,float r,float g,float b,float a) {
    pUniform1i(u_crt_preview,0);
    pUniform1i(u_mode,1);
    set_rect_virtual(x,y,w,h);
    pUniform4f(u_tint,r,g,b,a);
    glDrawArrays(GL_TRIANGLES,0,3);
}

void draw_tex_rect(const Tex &t,float x,float y,float w,float h,
                   float r=1,float g=1,float b=1,float a=1) {
    if (!t.id || t.w<=0 || t.h<=0) return;
    pUniform1i(u_mode,0);
    pUniform1i(u_crt_preview,0);
    set_rect_virtual(x,y,w,h);
    pUniform4f(u_tint,r,g,b,a);
    glBindTexture(GL_TEXTURE_2D,t.id);
    glDrawArrays(GL_TRIANGLES,0,3);
}

struct CrtProfile {
    float strength;
    float gamma;
};

CrtProfile crt_profile(int mode) {
    static const CrtProfile p[6] = {
        {0.00f, 1.00f}, /* off */
        {0.12f, 1.00f}, /* PVM - delikatny */
        {0.30f, 1.04f}, /* classic */
        {0.48f, 1.08f}, /* consumer TV */
        {0.68f, 1.10f}, /* arcade */
        {0.90f, 1.14f}, /* strong */
    };
    return p[std::clamp(mode,0,5)];
}

void draw_tex_rect_crt(const Tex &t,float x,float y,float w,float h,int crt_mode) {
    if (!t.id || t.w<=0 || t.h<=0) return;
    CrtProfile cp=crt_profile(crt_mode);
    pUniform1i(u_mode,0);
    pUniform1i(u_crt_preview,crt_mode>0?1:0);
    pUniform1f(u_crt_strength,cp.strength);
    pUniform1f(u_crt_gamma,cp.gamma);
    pUniform1f(u_crt_lines,240.0f);
    set_rect_virtual(x,y,w,h);
    pUniform4f(u_tint,1,1,1,1);
    glBindTexture(GL_TEXTURE_2D,t.id);
    glDrawArrays(GL_TRIANGLES,0,3);
    pUniform1i(u_crt_preview,0);
}

void draw_tex_height(const Tex &t,float x,float y,float height,bool selected) {
    if (!t.id || t.h<=0) return;
    float scale=selected?1.08f:1.0f;
    float h=height*scale;
    float w=h*(float)t.w/(float)t.h;
    float dy=(height-h)*0.5f;
    if (selected) {
        draw_tex_rect(t,x+5,y+5+dy,w,h,0,0,0,0.70f);
        draw_tex_rect(t,x,y+dy,w,h,1.00f,0.72f,0.12f,1.0f);
    } else {
        draw_tex_rect(t,x,y,w,h,0.94f,0.94f,0.92f,0.96f);
    }
}

void draw_circle_mask(float radius,float cx=0.5f,float cy=0.5f) {
    glDisable(GL_SCISSOR_TEST);
    pUniform1i(u_crt_preview,0);
    int ww=1,wh=1;
    SDL_GL_GetDrawableSize(g_window,&ww,&wh);
    float aspect=(wh>0)?(float)ww/(float)wh:1.7777f;
    pUniform1i(u_mode,2);
    pUniform2f(u_circle_center,cx,cy);
    pUniform1f(u_circle_radius,radius);
    pUniform1f(u_aspect,aspect);
    pUniform4f(u_tint,0,0,0,1);
    glDrawArrays(GL_TRIANGLES,0,3);
}

float max_circle_radius() {
    int ww=1,wh=1;
    SDL_GL_GetDrawableSize(g_window,&ww,&wh);
    float aspect=(wh>0)?(float)ww/(float)wh:1.7777f;
    return std::sqrt(0.25f*aspect*aspect+0.25f)*1.08f;
}

float ease(float t) {
    t=std::clamp(t,0.0f,1.0f);
    return t*t*(3.0f-2.0f*t);
}

/* ---- Window + host settings -------------------------------------------- */
void resolution_wh(int idx,int &w,int &h) {
    static const int W[3]={1280,1600,1920};
    static const int H[3]={720,900,1080};
    if (idx>=0 && idx<3) { w=W[idx]; h=H[idx]; return; }
#if defined(PSX_SDL3)
    SDL_DisplayID d=SDL_GetDisplayForWindow(g_window);
    SDL_DisplayMode m{};
    if (SDL_GetCurrentDisplayMode(d,&m)==0) {
        w=m.w; h=m.h; return;
    }
#else
    int di=SDL_GetWindowDisplayIndex(g_window);
    SDL_DisplayMode m{};
    if (di>=0 && SDL_GetCurrentDisplayMode(di,&m)==0) { w=m.w; h=m.h; return; }
#endif
    w=1920; h=1080;
}

void apply_window_settings() {
    /* MICKEY_PAUSE_WINDOW_SYNC_RESOLUTION_FIX_V3
     *
     * SDL3 window/fullscreen requests are asynchronous.
     *
     * Old code did:
     *   fullscreen(false) -> SetWindowSize(...)
     * without waiting for LEAVE_FULLSCREEN, so SetWindowSize could be ignored
     * because SDL still considered the window fullscreen.
     *
     * It also did:
     *   fullscreen(true) -> immediately return to GL pause rendering
     * while ENTER_FULLSCREEN / drawable recreation was still pending.
     *
     * That explains both symptoms:
     *   - fullscreen -> window looked OK, but resolution did not change;
     *   - window -> fullscreen could crash the pause/game renderer.
     *
     * v3 makes every state transition synchronous before GL continues.
     */
    int w=1600,h=900;
    resolution_wh(g_cfg.resolution,w,h);

#if defined(PSX_SDL3)
    auto sync_window=[&](const char *where) {
        SDL_PumpEvents();
        const bool ok=SDL_SyncWindow(g_window);
        SDL_PumpEvents();

        int aw=0,ah=0,pw=0,ph=0;
        SDL_GetWindowSize(g_window,&aw,&ah);
        SDL_GL_GetDrawableSize(g_window,&pw,&ph);

        std::fprintf(stdout,
            "MICKEY_PAUSE_WINDOW_SYNC_RESOLUTION_FIX_V3: %s "
            "sync=%d requested=%dx%d actual=%dx%d drawable=%dx%d mode=%d res=%d\n",
            where,ok?1:0,w,h,aw,ah,pw,ph,g_cfg.window_mode,g_cfg.resolution);
        std::fflush(stdout);
    };

    if (g_cfg.window_mode==0) {
        /* WINDOWED:
         * first completely leave fullscreen, THEN resize.
         * SDL_SetWindowSize has no effect while fullscreen. */
        SDL_SetWindowFullscreen(g_window,false);
        sync_window("leave-fullscreen-for-windowed");

        SDL_SetWindowFullscreenMode(g_window,nullptr);
        SDL_SetWindowBordered(g_window,true);
        SDL_SetWindowSize(g_window,w,h);
        sync_window("windowed-resize");

        SDL_SetWindowPosition(
            g_window,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED);
        sync_window("windowed-center");

    } else if (g_cfg.window_mode==1) {
        /* BORDERLESS:
         * 720p/900p/1080p = centered borderless window of that size.
         * DESKTOP = true borderless fullscreen desktop.
         *
         * This makes the Resolution option visibly useful in Borderless too. */
        if (g_cfg.resolution==3) {
            SDL_SetWindowFullscreenMode(g_window,nullptr);
            SDL_SetWindowFullscreen(g_window,true);
            sync_window("borderless-desktop");
        } else {
            SDL_SetWindowFullscreen(g_window,false);
            sync_window("leave-fullscreen-for-borderless-window");

            SDL_SetWindowFullscreenMode(g_window,nullptr);
            SDL_SetWindowBordered(g_window,false);
            SDL_SetWindowSize(g_window,w,h);
            sync_window("borderless-resize");

            SDL_SetWindowPosition(
                g_window,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED);
            sync_window("borderless-center");
        }

    } else {
        /* EXCLUSIVE FULLSCREEN:
         * The old code always reused the CURRENT desktop display mode, so the
         * Resolution selector was ignored. Pick the closest real fullscreen
         * mode to 720p/900p/1080p instead. DESKTOP keeps current mode. */
        SDL_DisplayID d=SDL_GetDisplayForWindow(g_window);
        SDL_DisplayMode m{};
        bool have_mode=false;

        if (g_cfg.resolution==3) {
            have_mode=(SDL_GetCurrentDisplayMode(d,&m)==0);
        } else {
            have_mode=SDL_GetClosestFullscreenDisplayMode(
                d,w,h,0.0f,false,&m);
        }

        if (have_mode) {
            SDL_SetWindowFullscreenMode(g_window,&m);
        } else {
            std::fprintf(stderr,
                "MICKEY_PAUSE_WINDOW_SYNC_RESOLUTION_FIX_V3: "
                "no exclusive %dx%d mode: %s; fallback borderless desktop\n",
                w,h,SDL_GetError());
            std::fflush(stderr);
            SDL_SetWindowFullscreenMode(g_window,nullptr);
        }

        SDL_SetWindowFullscreen(g_window,true);
        sync_window("exclusive-fullscreen");
    }

    /* Reassert GL ownership after Windows/SDL completed the transition. */
    SDL_GLContext ctx=SDL_GL_GetCurrentContext();
    if (ctx)
        SDL_GL_MakeCurrent(g_window,ctx);

    SDL_PumpEvents();

#else
    /* SDL2 fallback: preserve previous behavior. */
    if (g_cfg.window_mode==0) {
        SDL_SetWindowFullscreen(g_window,0);
        SDL_SetWindowBordered(g_window,SDL_TRUE);
        SDL_SetWindowSize(g_window,w,h);
        SDL_SetWindowPosition(g_window,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED);
    } else if (g_cfg.window_mode==1) {
        SDL_SetWindowFullscreen(g_window,SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        int di=SDL_GetWindowDisplayIndex(g_window);
        SDL_DisplayMode m{};
        if (di>=0 && SDL_GetCurrentDisplayMode(di,&m)==0) {
            if (g_cfg.resolution>=0 && g_cfg.resolution<3) {
                m.w=w;
                m.h=h;
            }
            SDL_SetWindowDisplayMode(g_window,&m);
        }
        SDL_SetWindowFullscreen(g_window,SDL_WINDOW_FULLSCREEN);
    }
#endif
}

/* MICKEY_DISPLAY_LIVEFIX_V1
 * MICKEY_DISPLAY_LIVEFIX_V1_1_COMPILEFIX
 *
 * OFF worked because it cleared the renderer directly. Switching 01..04 used
 * the mod-resource path, which is not a reliable live texture replacement.
 * Decode Border/NN.png and replace the renderer texture directly each time.
 *
 * CRT preset changes reset the old presentation state, then apply BOTH
 * scanline strength and post-gamma. Thus non-zero -> non-zero is a real change.
 */
static bool mickey_apply_bezel_direct(int bezel_index) {
    bezel_index=std::clamp(bezel_index,0,4);

    if (bezel_index==0)
        return gl_renderer_set_bezel(nullptr,0,0)!=0;

    char name[16];
    std::snprintf(name,sizeof(name),"%02d.png",bezel_index);

    fs::path path=exe_dir()/"Border"/name;
    std::error_code ec;
    if (!fs::is_regular_file(path,ec))
        path=fs::current_path()/"Border"/name;

    std::vector<unsigned char> bytes;
    if (!fs::is_regular_file(path,ec) || !read_file(path,bytes)) {
        std::fprintf(stderr,
            "mickey_frontend: DISPLAY LIVEFIX bezel %d missing: %s\n",
            bezel_index,path.string().c_str());
        std::fflush(stderr);
        gl_renderer_set_bezel(nullptr,0,0);
        return false;
    }

    int w=0,h=0,comp=0;
    unsigned char *rgba=stbi_load_from_memory(
        bytes.data(),(int)bytes.size(),&w,&h,&comp,4);
    if (!rgba || w<=0 || h<=0) {
        std::fprintf(stderr,
            "mickey_frontend: DISPLAY LIVEFIX bezel %d decode failed: %s\n",
            bezel_index,path.string().c_str());
        std::fflush(stderr);
        if (rgba) stbi_image_free(rgba);
        gl_renderer_set_bezel(nullptr,0,0);
        return false;
    }

    const int ok=gl_renderer_set_bezel(rgba,w,h);
    stbi_image_free(rgba);

    std::fprintf(stdout,
        "mickey_frontend: DISPLAY LIVEFIX bezel=%d file=%s size=%dx%d applied=%d\n",
        bezel_index,path.string().c_str(),w,h,ok?1:0);
    std::fflush(stdout);
    return ok!=0;
}

void apply_game_settings() {
    const int mode=std::clamp(g_cfg.crt_mode,0,5);
    const CrtProfile cp=crt_profile(mode);

    gl_renderer_set_scanlines(0,0.0f);

    if (mode>0) {
        gl_renderer_set_scanlines(1,cp.strength);
    }

    const bool bezel_ok=mickey_apply_bezel_direct(g_cfg.bezel);

    std::fprintf(stdout,
        "mickey_frontend: DISPLAY LIVEFIX v1.1 crt=%d scanlines=%d strength=%.3f bezel=%d bezel_ok=%d\n",
        mode,mode>0?1:0,mode>0?cp.strength:0.0f,
        g_cfg.bezel,bezel_ok?1:0);
    std::fflush(stdout);
}

/* MICKEY_RA_I18N_V1 */
/* ---- Menu + story profiles ---------------------------------------------- */
enum class Page { Main, ProfileNew, ProfileContinue, Achievements, DisplaySettings, Controls };
const char* mickey_input_legend(bool with_lr);

void render_common() {
    int ww=1,wh=1;
    gl_setup_frame(&ww,&wh);
    glClearColor(0.01f,0.01f,0.015f,1);
    glClear(GL_COLOR_BUFFER_BIT);

    draw_tex_rect(bg_tex,0,0,VW,VH,0.84f,0.84f,0.86f,1.0f);
    draw_solid(0,0,VW,VH,0,0,0,0.10f);
    draw_solid(0,0,790,VH,0.005f,0.008f,0.016f,0.74f);
    draw_solid(788,0,4,VH,1.0f,0.47f,0.04f,0.88f);

    if (logo_tex.id) {
        float w=600.0f;
        float h=w*(float)logo_tex.h/(float)logo_tex.w;
        draw_tex_rect(logo_tex,68,48,w,h,1,1,1,1);
    }
}

/* MICKEY_RA_I18N_V1_1_1_DRAW_TEXT_FORWARD */
float draw_front_text(const char *text,float x,float y,float height,
                      float r,float g,float b,float a);

void render_main(int selected) {
    render_common();
    const char* lab[5]={tr("main_new"),tr("main_continue"),tr("main_ach"),tr("main_settings"),tr("main_exit")};
    const float ys[5]={372,472,572,672,772};
    for(int i=0;i<5;i++){bool sel=i==selected;draw_front_text(lab[i],98,ys[i],43,sel?1.0f:0.90f,sel?0.62f:0.90f,sel?0.08f:0.88f,1);}
    draw_front_text(mickey_input_legend(false),98,900,16,0.62f,0.62f,0.60f,1.0f);
}

static uint32_t mickey_utf8_next(const unsigned char*& p) {
    uint32_t c=*p++; if(c<0x80)return c;
    if((c&0xE0)==0xC0 && p[0]) {uint32_t r=(c&0x1F)<<6; r|=*p++&0x3F; return r;}
    if((c&0xF0)==0xE0 && p[0]&&p[1]) {uint32_t r=(c&0x0F)<<12; r|=(p[0]&0x3F)<<6; r|=p[1]&0x3F; p+=2; return r;}
    if((c&0xF8)==0xF0 && p[0]&&p[1]&&p[2]) {uint32_t r=(c&7)<<18; r|=(p[0]&0x3F)<<12; r|=(p[1]&0x3F)<<6; r|=p[2]&0x3F; p+=3; return r;} return '?';
}
float draw_front_text(const char *text,float x,float y,float height,
                      float r=0.94f,float g=0.94f,float b=0.92f,float a=1.0f) {
    if(!text)return x; float pen=x; const unsigned char* p=(const unsigned char*)text;
    while(*p){uint32_t c=mickey_utf8_next(p); if(c==' '){pen+=height*0.42f;continue;} const Tex* t=nullptr;
        if(c<128 && glyph_tex[c].id)t=&glyph_tex[c]; else {auto it=glyph_unicode.find(c);if(it!=glyph_unicode.end()&&it->second.id)t=&it->second;}
        if(!t||t->h<=0){pen+=height*0.46f;continue;}float w=height*(float)t->w/(float)t->h;draw_tex_rect(*t,pen,y,w,height,r,g,b,a);pen+=w*0.86f;} return pen;
}

/* MICKEY_STARTUP_SEQUENCE_HOTFIX_V1_1 */
void present();

/* MICKEY_STARTUP_SEQUENCE_V1 ------------------------------------------------ */
/* MICKEY_STARTUP_UI_I18N_FIX_V1_3
 * Startup cards use their own tighter typography.
 * The Mickey display font has intentionally chunky punctuation; drawing
 * punctuation at the same height as capitals made commas/dots/apostrophes
 * look enormous and created fake gaps in names such as Mickey's.
 */
static bool startup_is_low_punct(uint32_t c) {
    return c=='.' || c==',' || c==';' || c==':' ||
           c=='!' || c=='?' || c==0x3001u || c==0x3002u;
}

static bool startup_is_high_punct(uint32_t c) {
    return c=='\'' || c=='"' || c==0x2018u || c==0x2019u ||
           c==0x201Cu || c==0x201Du;
}

static const Tex* startup_glyph(uint32_t c) {
    if (c<128 && glyph_tex[c].id) return &glyph_tex[c];
    auto it=glyph_unicode.find(c);
    return (it!=glyph_unicode.end() && it->second.id) ? &it->second : nullptr;
}

float draw_startup_text(const char *text,float x,float y,float height,
                        float r=0.94f,float g=0.94f,float b=0.92f,float a=1.0f) {
    if(!text) return x;
    float pen=x;
    const unsigned char* p=(const unsigned char*)text;

    while(*p) {
        const uint32_t c=mickey_utf8_next(p);

        if(c==' ') {
            pen += height*0.30f;
            continue;
        }

        const Tex* t=startup_glyph(c);
        if(!t || t->h<=0) {
            pen += height*0.38f;
            continue;
        }

        if (startup_is_high_punct(c)) {
            const float ph=height*0.38f;
            const float pw=ph*(float)t->w/(float)t->h;
            draw_tex_rect(*t,pen,y+height*0.02f,pw,ph,r,g,b,a);
            pen += std::max(pw*0.78f,height*0.13f);
            continue;
        }

        if (startup_is_low_punct(c)) {
            const float ph=height*0.43f;
            const float pw=ph*(float)t->w/(float)t->h;
            draw_tex_rect(*t,pen,y+height*0.52f,pw,ph,r,g,b,a);
            pen += std::max(pw*0.78f,height*0.16f);
            continue;
        }

        if (c=='-' || c==0x2013u || c==0x2014u) {
            const float ph=height*0.34f;
            const float pw=ph*(float)t->w/(float)t->h;
            draw_tex_rect(*t,pen,y+height*0.34f,pw,ph,r,g,b,a);
            pen += std::max(pw*0.84f,height*0.22f);
            continue;
        }

        const float w=height*(float)t->w/(float)t->h;
        draw_tex_rect(*t,pen,y,w,height,r,g,b,a);
        pen += w*0.80f;
    }
    return pen;
}

float measure_startup_text(const char *text,float height) {
    if(!text) return 0.0f;
    float pen=0.0f;
    const unsigned char* p=(const unsigned char*)text;

    while(*p) {
        const uint32_t c=mickey_utf8_next(p);
        if(c==' ') {
            pen += height*0.30f;
            continue;
        }

        const Tex* t=startup_glyph(c);
        if(!t || t->h<=0) {
            pen += height*0.38f;
            continue;
        }

        if (startup_is_high_punct(c)) {
            const float ph=height*0.38f;
            const float pw=ph*(float)t->w/(float)t->h;
            pen += std::max(pw*0.78f,height*0.13f);
        } else if (startup_is_low_punct(c)) {
            const float ph=height*0.43f;
            const float pw=ph*(float)t->w/(float)t->h;
            pen += std::max(pw*0.78f,height*0.16f);
        } else if (c=='-' || c==0x2013u || c==0x2014u) {
            const float ph=height*0.34f;
            const float pw=ph*(float)t->w/(float)t->h;
            pen += std::max(pw*0.84f,height*0.22f);
        } else {
            const float w=height*(float)t->w/(float)t->h;
            pen += w*0.80f;
        }
    }
    return pen;
}

void draw_centered_startup_text(const char *text,float y,float height,
                                float r=0.94f,float g=0.94f,float b=0.92f,float a=1.0f) {
    const float w=measure_startup_text(text,height);
    draw_startup_text(text,(VW-w)*0.5f,y,height,r,g,b,a);
}

float startup_fade_card(float t,float total,float fade=0.40f) {
    if (total<=0.0f) return 1.0f;
    const float fin=std::clamp(t/fade,0.0f,1.0f);
    const float fout=std::clamp((total-t)/fade,0.0f,1.0f);
    return std::min(fin,fout);
}

void startup_begin_frame_black() {
    int ww=1,wh=1;
    gl_setup_frame(&ww,&wh);
    glClearColor(0.0f,0.0f,0.0f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_solid(0,0,VW,VH,0.0f,0.0f,0.0f,1.0f);
}

void startup_draw_notice_lines(const char* const* lines,int count,float x,float y,float height,float step,float a) {
    for (int i=0;i<count;i++) {
        if (!lines[i] || !lines[i][0]) continue;
        draw_front_text(lines[i],x,y+i*step,height,0.92f,0.92f,0.90f,a);
    }
}

void startup_draw_warning(float alpha) {
    startup_begin_frame_black();

    const int lang=std::clamp(g_cfg.language,0,4);

    static const char* title[5] = {
        "UWAGA!",
        "WARNING!",
        "ATTENZIONE!",
        "注意!",
        "ACHTUNG!"
    };

    static const char* pl[] = {
        "Ten projekt jest całkowicie niekomercyjnym recompem stworzonym z miłości",
        "do Mickey's Wild Adventure. To mała laurka dla Disneya oraz Traveller's Tales,",
        "którzy wykonali przy tej grze naprawdę świetną robotę.",
        "",
        "Nie jestem powiązany, sponsorowany ani zatwierdzony przez The Walt Disney Company,",
        "Traveller's Tales, Sony ani żadną inną firmę związaną z oryginalnym wydaniem gry.",
        "",
        "Celem recompu jest umożliwienie fanom uruchomienia posiadanej przez nich gry",
        "na współczesnych i możliwie wielu urządzeniach. Projekt nie jest sprzedawany",
        "i nie ma na celu zastępowania oficjalnego wydania.",
        "",
        "Wesprzyj oryginalnych twórców i kup grę! Serio. Kup ją.",
        "Steam? Nie ma.   GOG? Nie ma.   Współczesny PlayStation Store? Powodzenia.",
        "",
        "Jeśli znajdziesz oficjalny sklep, który nadal sprzedaje Mickey's Wild Adventure,",
        "daj mi znać. Ja też chcę wiedzieć gdzie.",
        "",
        "Do tego czasu dbaj o swoją oryginalną płytę jak o relikwię.",
        "",
        "Mickey Mouse, Mickey's Wild Adventure oraz związane materiały należą",
        "do ich odpowiednich właścicieli."
    };

    static const char* en[] = {
        "This is a completely non-commercial recomp made out of love for",
        "Mickey's Wild Adventure. It is a small tribute to Disney and Traveller's Tales,",
        "who did a fantastic job on the original game.",
        "",
        "I am not affiliated with, sponsored by, or endorsed by The Walt Disney Company,",
        "Traveller's Tales, Sony, or any company connected to the original release.",
        "",
        "The purpose of this recomp is only to let fans run a copy they own",
        "on modern and as many devices as possible. The project is not sold",
        "and is not intended to replace an official release.",
        "",
        "Support the original creators and buy the game! Seriously. Buy it.",
        "Steam? Nope.   GOG? Nope.   Modern PlayStation Store? Good luck.",
        "",
        "If you find an official store that still sells Mickey's Wild Adventure,",
        "tell me. I want to know too.",
        "",
        "Until then, treat your original disc like a relic.",
        "",
        "Mickey Mouse, Mickey's Wild Adventure, and related materials belong",
        "to their respective owners."
    };

    static const char* it[] = {
        "Questo progetto è un recomp completamente non commerciale, creato per amore di",
        "Mickey's Wild Adventure. È un piccolo omaggio a Disney e Traveller's Tales,",
        "che hanno fatto un lavoro fantastico con il gioco originale.",
        "",
        "Non sono affiliato, sponsorizzato o approvato da The Walt Disney Company,",
        "Traveller's Tales, Sony o da altre aziende legate all'uscita originale.",
        "",
        "Lo scopo del recomp è solo permettere ai fan di eseguire la copia che possiedono",
        "su dispositivi moderni e sul maggior numero possibile di sistemi.",
        "Il progetto non è venduto e non sostituisce un'uscita ufficiale.",
        "",
        "Sostieni gli autori originali e compra il gioco! Sul serio. Compralo.",
        "Steam? Non c'è.   GOG? Non c'è.   PlayStation Store moderno? Buona fortuna.",
        "",
        "Se trovi un negozio ufficiale che vende ancora Mickey's Wild Adventure,",
        "fammi sapere. Voglio saperlo anch'io.",
        "",
        "Nel frattempo, tratta il tuo disco originale come una reliquia.",
        "",
        "Mickey Mouse, Mickey's Wild Adventure e i materiali correlati appartengono",
        "ai rispettivi proprietari."
    };

    static const char* jp[] = {
        "このプロジェクトは Mickey's Wild Adventure への愛から作られた完全非営利の recomp です。",
        "Disney と Traveller's Tales への小さなオマージュであり、",
        "オリジナル版への素晴らしい仕事に敬意を表します。",
        "",
        "The Walt Disney Company、Traveller's Tales、Sony、その他の関係企業とは",
        "提携、スポンサー関係、承認関係のいずれもありません。",
        "",
        "目的は、正規に所有しているゲームを現代のさまざまな端末で",
        "遊べるようにすることだけです。このプロジェクトは販売されず、",
        "公式版の代わりになることを目的としていません。",
        "",
        "オリジナルの制作者を応援して、ゲームを買おう！ 本当に。買おう。",
        "Steam？ ありません。   GOG？ ありません。   現在の PlayStation Store？ 健闘を祈ります。",
        "",
        "今でも公式に Mickey's Wild Adventure を販売している店を見つけたら、",
        "ぜひ教えてください。私も知りたいです。",
        "",
        "それまでは、オリジナルディスクを宝物のように大切に。",
        "",
        "Mickey Mouse、Mickey's Wild Adventure および関連素材の権利は",
        "それぞれの権利者に帰属します。"
    };

    static const char* de[] = {
        "Dieses Projekt ist ein vollständig nichtkommerzielles Recomp, erstellt aus Liebe zu",
        "Mickey's Wild Adventure. Es ist eine kleine Hommage an Disney und Traveller's Tales,",
        "die beim Originalspiel großartige Arbeit geleistet haben.",
        "",
        "Ich bin weder mit The Walt Disney Company, Traveller's Tales oder Sony verbunden",
        "noch von ihnen gesponsert oder bestätigt.",
        "",
        "Dieses Recomp soll Fans lediglich ermöglichen, ihre eigene Spielkopie",
        "auf modernen und möglichst vielen Geräten auszuführen. Das Projekt wird nicht verkauft",
        "und soll keine offizielle Veröffentlichung ersetzen.",
        "",
        "Unterstütze die Originalentwickler und kauf das Spiel! Wirklich. Kauf es.",
        "Steam? Gibt es nicht.   GOG? Gibt es nicht.   Moderner PlayStation Store? Viel Glück.",
        "",
        "Falls du einen offiziellen Shop findest, der Mickey's Wild Adventure noch verkauft,",
        "sag mir Bescheid. Ich möchte es auch wissen.",
        "",
        "Bis dahin: Behandle deine Originaldisc wie eine Reliquie.",
        "",
        "Mickey Mouse, Mickey's Wild Adventure und zugehörige Materialien gehören",
        "den jeweiligen Rechteinhabern."
    };

    const char* const* lines=pl;
    int count=(int)(sizeof(pl)/sizeof(pl[0]));
    if (lang==1) { lines=en; count=(int)(sizeof(en)/sizeof(en[0])); }
    else if (lang==2) { lines=it; count=(int)(sizeof(it)/sizeof(it[0])); }
    else if (lang==3) { lines=jp; count=(int)(sizeof(jp)/sizeof(jp[0])); }
    else if (lang==4) { lines=de; count=(int)(sizeof(de)/sizeof(de[0])); }

    draw_centered_startup_text(title[lang],64,44,1.0f,0.62f,0.08f,alpha);

    const float panel_x=170.0f,panel_y=128.0f,panel_w=1580.0f,panel_h=770.0f;
    draw_solid(panel_x,panel_y,panel_w,panel_h,0.012f,0.014f,0.022f,0.84f*alpha);
    draw_solid(panel_x,panel_y,panel_w,4,1.0f,0.48f,0.04f,0.96f*alpha);

    const float h=(lang==3)?18.0f:19.0f;
    const float step=(lang==3)?30.0f:31.0f;
    float y=165.0f;
    for(int i=0;i<count;i++) {
        if (!lines[i][0]) {
            y += step*0.48f;
            continue;
        }
        draw_centered_startup_text(lines[i],y,h,0.92f,0.92f,0.90f,alpha);
        y += step;
    }

    static const char* press[5] = {
        "NACIŚNIJ DOWOLNY PRZYCISK, ABY KONTYNUOWAĆ",
        "PRESS ANY BUTTON TO CONTINUE",
        "PREMI UN TASTO QUALSIASI PER CONTINUARE",
        "どれかのボタンを押して続けてください",
        "BELIEBIGE TASTE DRÜCKEN, UM FORTZUFAHREN"
    };
    draw_centered_startup_text(press[lang],954,18,0.72f,0.72f,0.70f,alpha);
}

void startup_draw_save_notice(float alpha,uint64_t now) {
    startup_begin_frame_black();

    const int lang=std::clamp(g_cfg.language,0,4);

    static const char* heading[5] = {
        "UWAGA - ZAPISYWANIE",
        "WARNING - SAVING",
        "ATTENZIONE - SALVATAGGIO",
        "注意 - セーブ中",
        "ACHTUNG - SPEICHERN"
    };
    static const char* line1[5] = {
        "Jeśli zobaczysz ten symbol, gra zapisuje postęp.",
        "When this symbol appears, the game is saving your progress.",
        "Quando compare questo simbolo, il gioco sta salvando i progressi.",
        "このマークが表示されている間、ゲームは進行状況を保存しています。",
        "Wenn dieses Symbol erscheint, speichert das Spiel deinen Fortschritt."
    };
    static const char* line2[5] = {
        "Nie wyłączaj urządzenia ani nie zamykaj gry, dopóki symbol nie zniknie.",
        "Do not turn off the device or close the game until the symbol disappears.",
        "Non spegnere il dispositivo e non chiudere il gioco finché il simbolo non scompare.",
        "マークが消えるまで、端末の電源を切ったりゲームを終了したりしないでください。",
        "Schalte das Gerät nicht aus und beende das Spiel nicht, solange das Symbol sichtbar ist."
    };
    static const char* line3[5] = {
        "NIE WYŁĄCZAJ URZĄDZENIA, KIEDY TEN SYMBOL JEST WIDOCZNY",
        "DO NOT TURN OFF THE DEVICE WHILE THIS SYMBOL IS VISIBLE",
        "NON SPEGNERE IL DISPOSITIVO MENTRE QUESTO SIMBOLO È VISIBILE",
        "このマークが表示中は端末の電源を切らないでください",
        "GERÄT NICHT AUSSCHALTEN, SOLANGE DIESES SYMBOL SICHTBAR IST"
    };
    static const char* press[5] = {
        "NACIŚNIJ DOWOLNY PRZYCISK, ABY KONTYNUOWAĆ",
        "PRESS ANY BUTTON TO CONTINUE",
        "PREMI UN TASTO QUALSIASI PER CONTINUARE",
        "どれかのボタンを押して続けてください",
        "BELIEBIGE TASTE DRÜCKEN, UM FORTZUFAHREN"
    };

    draw_centered_startup_text(heading[lang],142,40,1.0f,0.62f,0.08f,alpha);
    draw_centered_startup_text(line1[lang],244,(lang==3)?19.0f:21.0f,0.92f,0.92f,0.90f,alpha);
    draw_centered_startup_text(line2[lang],284,(lang==3)?17.0f:19.0f,0.82f,0.82f,0.80f,alpha);

    const float x=520.0f,y=392.0f,w=880.0f,h=192.0f;
    draw_solid(x,y,w,h,0.012f,0.016f,0.026f,0.90f*alpha);
    draw_solid(x,y,w,4,1.0f,0.47f,0.04f,0.96f*alpha);
    draw_solid(x,y+h-3,w,3,0.28f,0.28f,0.34f,0.86f*alpha);
    draw_solid(x,y,3,h,0.28f,0.28f,0.34f,0.86f*alpha);
    draw_solid(x+w-3,y,3,h,0.28f,0.28f,0.34f,0.86f*alpha);

    /* MICKEY_STARTUP_SAVE_BADGE_I18N_V1_4
     * saving.png zawiera polskie "ZAPISYWANIE...", wiec NIE rysujemy
     * tej tekstury na planszy informacyjnej. Zamiast niej ten sam badge
     * jest skladany z glyphow i bierze jezyk z g_cfg.language.
     */
    static const char* save_badge[5] = {
        "ZAPISYWANIE...",
        "SAVING...",
        "SALVATAGGIO...",
        "セーブ中...",
        "SPEICHERN..."
    };
    {
        const float badge_h=(lang==3)?30.0f:34.0f;
        const float badge_center=x+300.0f;
        const float badge_w=measure_startup_text(save_badge[lang],badge_h);
        draw_startup_text(
            save_badge[lang],
            badge_center-badge_w*0.5f,
            y+72.0f,
            badge_h,
            0.96f,0.96f,0.94f,alpha);
    }

    const int hot=(int)((now/90u)%8u);
    const float cx=x+w-82.0f, cy=y+h*0.5f;
    for(int i=0;i<8;i++) {
        const float ang=(float)i*6.28318530718f/8.0f;
        const float sx=cx+std::cos(ang)*30.0f-6.0f;
        const float sy=cy+std::sin(ang)*30.0f-6.0f;
        if (i==hot) draw_solid(sx,sy,12,12,1.0f,0.55f,0.08f,alpha);
        else draw_solid(sx,sy,9,9,0.86f,0.86f,0.82f,0.42f*alpha);
    }

    draw_centered_startup_text(line3[lang],658,(lang==3)?18.0f:20.0f,0.92f,0.92f,0.90f,alpha);
    draw_centered_startup_text(press[lang],936,18,0.72f,0.72f,0.70f,alpha);
}

void startup_draw_kacperek(float t,float alpha) {
    startup_begin_frame_black();

    const int lang=std::clamp(g_cfg.language,0,4);
    static const char* present[5] = {
        "KACPEREK PREZENTUJE",
        "KACPEREK PRESENTS",
        "KACPEREK PRESENTA",
        "KACPEREK プレゼンツ",
        "KACPEREK PRÄSENTIERT"
    };
    static const char* wild[5] = {
        "NAJBARDZIEJ WILD DECOMP, JAKI KIEDYKOLWIEK POWSTAŁ",
        "THE MOST WILD DECOMP EVER MADE",
        "IL RECOMP PIÙ WILD MAI CREATO",
        "史上もっとも WILD な RECOMP",
        "DAS WILDESTE RECOMP ALLER ZEITEN"
    };

    draw_centered_startup_text(present[lang],430,(lang==3)?50.0f:60.0f,1.0f,0.90f,0.82f,alpha);

    if (t>=0.826f) {
        const float sub_a=(t<1.15f)
            ? std::clamp((t-0.826f)/0.28f,0.0f,1.0f)*alpha
            : alpha;
        draw_centered_startup_text(wild[lang],536,(lang==3)?28.0f:32.0f,1.0f,0.62f,0.08f,sub_a);
    }
}

void startup_draw_title_idle(float t) {
    int ww=1,wh=1;
    gl_setup_frame(&ww,&wh);
    glClearColor(0.01f,0.01f,0.015f,1);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_tex_rect(bg_tex,0,0,VW,VH,0.84f,0.84f,0.86f,1.0f);
    draw_solid(0,0,VW,VH,0,0,0,0.22f);

    const float a=std::clamp(t/0.36f,0.0f,1.0f);
    const float e=ease(a);
    const float w=820.0f + 120.0f*(1.0f-e);
    const float h=w*(float)logo_tex.h/(float)std::max(1,logo_tex.w);
    const float x=(VW-w)*0.5f;
    const float y=292.0f - 24.0f*(1.0f-e);
    if (logo_tex.id)
        draw_tex_rect(logo_tex,x,y,w,h,1,1,1,a);

    if (t>=0.90f) {
        const int lang=std::clamp(g_cfg.language,0,4);
        static const char* press[5] = {
            "WCIŚNIJ JAKIKOLWIEK PRZYCISK",
            "PRESS ANY BUTTON",
            "PREMI UN TASTO QUALSIASI",
            "どれかのボタンを押してください",
            "BELIEBIGE TASTE DRÜCKEN"
        };
        const bool blink=((SDL_GetTicks()/420u)&1u)==0u;
        const float pa=blink?1.0f:0.54f;
        draw_centered_startup_text(press[lang],812,(lang==3)?23.0f:27.0f,1.0f,0.86f,0.82f,pa);
    }
}

void startup_draw_menu_blend(float t,int selected) {
    int ww=1,wh=1;
    gl_setup_frame(&ww,&wh);
    glClearColor(0.01f,0.01f,0.015f,1);
    glClear(GL_COLOR_BUFFER_BIT);

    draw_tex_rect(bg_tex,0,0,VW,VH,0.84f,0.84f,0.86f,1.0f);
    draw_solid(0,0,VW,VH,0,0,0,0.10f);
    draw_solid(0,0,790,VH,0.005f,0.008f,0.016f,0.74f);
    draw_solid(788,0,4,VH,1.0f,0.47f,0.04f,0.88f);

    const float e=ease(std::clamp(t,0.0f,1.0f));
    const float x0=(VW-900.0f)*0.5f;
    const float y0=292.0f;
    const float x1=68.0f;
    const float y1=48.0f;
    const float w0=900.0f;
    const float w1=600.0f;
    const float x=x0+(x1-x0)*e;
    const float y=y0+(y1-y0)*e;
    const float w=w0+(w1-w0)*e;
    const float h=w*(float)logo_tex.h/(float)std::max(1,logo_tex.w);
    if (logo_tex.id) draw_tex_rect(logo_tex,x,y,w,h,1,1,1,1);

    const char* lab[5]={tr("main_new"),tr("main_continue"),tr("main_ach"),tr("main_settings"),tr("main_exit")};
    const float ys[5]={372,472,572,672,772};
    const float mx=-360.0f + (98.0f+360.0f)*e;
    const float ma=std::clamp((t-0.15f)/0.60f,0.0f,1.0f);
    for(int i=0;i<5;i++){bool sel=i==selected;draw_front_text(lab[i],mx,ys[i],43,sel?1.0f:0.90f,sel?0.62f:0.90f,sel?0.08f:0.88f,ma);}
    draw_front_text(mickey_input_legend(false),mx,900,16,0.62f,0.62f,0.60f,ma);
}

bool startup_poll_any_press() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
#if defined(PSX_SDL3)
        if (e.type==SDL_EVENT_QUIT) std::exit(0);
        if (e.type==SDL_EVENT_MOUSE_BUTTON_DOWN) return true;
        if (e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) return true;
        if (e.type==SDL_EVENT_KEY_DOWN && !e.key.repeat) return true;
#else
        if (e.type==SDL_QUIT) std::exit(0);
        if (e.type==SDL_MOUSEBUTTONDOWN) return true;
        if (e.type==SDL_CONTROLLERBUTTONDOWN) return true;
        if (e.type==SDL_KEYDOWN && !e.key.repeat) return true;
#endif
    }
    return false;
}

void startup_wait_release_brief() {
    const uint64_t until=SDL_GetTicks()+180u;
    while (SDL_GetTicks()<until) {
        /* The menu track starts just before this wait. Its queued 3x1024
         * frames cover only ~70 ms, so keep feeding SDL during the 180 ms
         * release guard without changing the guard or startup timing. */
        menu_audio_pump();
        SDL_PumpEvents();
        SDL_Delay(4);
    }
}

void run_startup_sequence(int selected) {
    auto run_card=[&](auto draw_fn,float total_sec,float min_skip_sec=0.75f) {
        const uint64_t s=SDL_GetTicks();
        for (;;) {
            menu_audio_pump();
            const uint64_t now=SDL_GetTicks();
            const float t=(float)(now-s)/1000.0f;
            const float a=startup_fade_card(t,total_sec,0.42f);
            draw_fn(t,a,now);
            present();
            const bool pressed=startup_poll_any_press();
            if ((pressed && t>=min_skip_sec) || t>=total_sec) {
                if (pressed) menu_audio_pickup();
                startup_wait_release_brief();
                break;
            }
            frontend_present_idle(8);
        }
    };

    run_card([&](float, float a, uint64_t){ startup_draw_warning(a); }, 12.0f, 0.90f);
    run_card([&](float, float a, uint64_t now){ startup_draw_save_notice(a,now); }, 5.6f, 0.60f);

    /* Well Done = TYLKO intro/title, jako ONE-SHOT.
     * Ustaw stan PRZED otwarciem audio, żeby pierwsze zakolejkowane sample
     * były title fanfare, a nie muzyką menu. */
    g_menu_music_enabled=false;
    g_title_music_active=true;
    g_title_music_pos=0;
    menu_audio_start();

    {
        const uint64_t s=SDL_GetTicks();
        const float total=1.862f; /* dokładny punkt BOOM / WHITE FLASH */
        for (;;) {
            menu_audio_pump();
            const uint64_t now=SDL_GetTicks();
            const float t=(float)(now-s)/1000.0f;
            const float in=std::clamp(t/0.12f,0.0f,1.0f);
            startup_draw_kacperek(t,in);
            present();
            if (t>=total) break;
            startup_poll_any_press();
            frontend_present_idle(4);
        }
    }

    {
        const uint64_t s=SDL_GetTicks();
        for (;;) {
            menu_audio_pump();
            const float t=(float)(SDL_GetTicks()-s)/1000.0f;
            const float f=1.0f-std::clamp(t/0.18f,0.0f,1.0f);
            startup_draw_title_idle(0.0f);
            draw_solid(0,0,VW,VH,1.0f,1.0f,1.0f,f);
            present();
            if (t>=0.18f) break;
            frontend_present_idle(8);
        }
    }

    {
        const uint64_t s=SDL_GetTicks();
        for (;;) {
            menu_audio_pump();
            const float t=(float)(SDL_GetTicks()-s)/1000.0f;
            startup_draw_title_idle(t);
            present();
            if (startup_poll_any_press() && t>=0.40f) {
                /* Koniec title music. Nie zostawiamy jego bufora na menu. */
#if defined(PSX_SDL3)
                if (g_menu_audio_stream) SDL_ClearAudioStream(g_menu_audio_stream);
#endif
                g_title_music_active=false;
                g_title_music_pos=0;

                /* Tutaj dopiero startuje Heavenly menu track — od początku. */
                g_menu_music_enabled=true;
                g_music_pos=0;

                /* Ten sam dźwięk wyboru co zawsze. */
                menu_audio_enter();
                menu_audio_pump();
                startup_wait_release_brief();
                break;
            }
            frontend_present_idle(8);
        }
    }

    {
        const uint64_t s=SDL_GetTicks();
        const float total=0.72f;
        for (;;) {
            menu_audio_pump();
            const float t=(float)(SDL_GetTicks()-s)/(total*1000.0f);
            startup_draw_menu_blend(t,selected);
            present();
            if (t>=1.0f) break;
            startup_poll_any_press();
            frontend_present_idle(8);
        }
    }
}

/* MICKEY_RA_I18N_V1_1_ACH_LOGIN_UI */
struct RALoginOverlay {
    bool open=false;
    int focus=0; /* 0 user, 1 pass, 2 submit, 3 cancel */
    bool submitted=false;
    std::string user;
    std::string pass;
};
RALoginOverlay g_ra_login_overlay;
int g_ra_first_visible=0;
void mouse_virtual(float mx,float my,float &vx,float &vy);

const char* ra_ui_text(const char* key) {
    const int lang=std::clamp(g_cfg.language,0,4);
    if (std::strcmp(key,"login_title")==0) {
        static const char* t[5] = {"LOGOWANIE RETROACHIEVEMENTS","RETROACHIEVEMENTS SIGN IN","ACCESSO RETROACHIEVEMENTS","RETROACHIEVEMENTS ログイン","RETROACHIEVEMENTS-ANMELDUNG"}; return t[lang];
    }
    if (std::strcmp(key,"login_sub")==0) {
        static const char* t[5] = {"Zaloguj się bezpośrednio w grze.","Sign in directly inside the game.","Accedi direttamente nel gioco.","ゲーム内で直接ログインします。","Direkt im Spiel anmelden."}; return t[lang];
    }
    if (std::strcmp(key,"user")==0) {
        static const char* t[5] = {"Nazwa użytkownika","Username","Nome utente","ユーザー名","Benutzername"}; return t[lang];
    }
    if (std::strcmp(key,"pass")==0) {
        static const char* t[5] = {"Hasło","Password","Password","パスワード","Passwort"}; return t[lang];
    }
    if (std::strcmp(key,"submit")==0) {
        static const char* t[5] = {"Zaloguj","Sign In","Accedi","ログイン","Anmelden"}; return t[lang];
    }
    if (std::strcmp(key,"cancel")==0) {
        static const char* t[5] = {"Anuluj","Cancel","Annulla","キャンセル","Abbrechen"}; return t[lang];
    }
    if (std::strcmp(key,"secret")==0) {
        static const char* t[5] = {"Hasło nie jest zapisywane.","Your password is not stored.","La password non viene salvata.","パスワードは保存されません。","Das Passwort wird nicht gespeichert."}; return t[lang];
    }
    if (std::strcmp(key,"need_login")==0) {
        static const char* t[5] = {"Zaloguj się, aby zsynchronizować osiągnięcia z kontem.","Sign in to sync achievements with your account.","Accedi per sincronizzare gli obiettivi con il tuo account.","実績をアカウントと同期するにはログインしてください。","Melde dich an, um Erfolge mit deinem Konto zu synchronisieren."}; return t[lang];
    }
    if (std::strcmp(key,"account")==0) {
        static const char* t[5] = {"Konto","Account","Account","アカウント","Konto"}; return t[lang];
    }
    if (std::strcmp(key,"summary")==0) {
        static const char* t[5] = {"Podsumowanie","Summary","Riepilogo","概要","Übersicht"}; return t[lang];
    }
    if (std::strcmp(key,"locked")==0) {
        static const char* t[5] = {"Zablokowane","Locked","Bloccati","未解除","Gesperrt"}; return t[lang];
    }
    if (std::strcmp(key,"unlocked")==0) {
        static const char* t[5] = {"Odblokowane","Unlocked","Sbloccati","解除済み","Freigeschaltet"}; return t[lang];
    }
    if (std::strcmp(key,"type_here")==0) {
        static const char* t[5] = {"Wpisz tutaj...","Type here...","Scrivi qui...","ここに入力...","Hier eingeben..."}; return t[lang];
    }
    if (std::strcmp(key,"list_hint")==0) {
        static const char* t[5] = {"Najeżdżaj lub użyj strzałek. Enter: konto. Esc: powrót.","Hover or use arrows. Enter: account. Esc: back.","Passa sopra o usa le frecce. Invio: account. Esc: indietro.","ホバーまたは矢印で選択。Enter: アカウント。Esc: 戻る。","Mit Maus oder Pfeilen wählen. Enter: Konto. Esc: zurück."}; return t[lang];
    }
    if (std::strcmp(key,"status_idle")==0) {
        static const char* t[5] = {"Zaloguj się, aby rozpocząć synchronizację osiągnięć.","Sign in to begin syncing achievements.","Accedi per iniziare a sincronizzare gli obiettivi.","実績の同期を始めるにはログインしてください。","Melde dich an, um die Erfolgssynchronisierung zu starten."}; return t[lang];
    }
    if (std::strcmp(key,"sign_out_short")==0) {
        static const char* t[5] = {"Wyloguj","Sign Out","Esci","ログアウト","Abmelden"}; return t[lang];
    }
    if (std::strcmp(key,"page_short")==0) {
        static const char* t[5] = {"Strona","Page","Pagina","ページ","Seite"}; return t[lang];
    }
    if (std::strcmp(key,"preview_short")==0) {
        static const char* t[5] = {"Podgląd","Preview","Anteprima","プレビュー","Vorschau"}; return t[lang];
    }
    return "";
}

void ra_text_input_start() {
#if defined(PSX_SDL3)
    SDL_StartTextInput(g_window);
#else
    SDL_StartTextInput();
#endif
}
void ra_text_input_stop() {
#if defined(PSX_SDL3)
    SDL_StopTextInput(g_window);
#else
    SDL_StopTextInput();
#endif
}

bool in_rect(float mx,float my,float x,float y,float w,float h){return mx>=x&&mx<=x+w&&my>=y&&my<=y+h;}

void draw_frame_panel(float x,float y,float w,float h,float a=0.84f,bool accent=false){
    draw_solid(x,y,w,h,0.010f,0.014f,0.022f,a);
    const float br=accent?1.0f:0.36f, bg=accent?0.58f:0.36f, bb=accent?0.10f:0.42f;
    draw_solid(x,y,w,3,br,bg,bb,0.96f);
    draw_solid(x,y+h-3,w,3,br,bg,bb,0.96f);
    draw_solid(x,y,3,h,br,bg,bb,0.96f);
    draw_solid(x+w-3,y,3,h,br,bg,bb,0.96f);
}

void draw_action_button(float x,float y,float w,float h,const char* label,bool selected,bool danger=false){
    draw_solid(x,y,w,h,selected?0.14f:0.04f,selected?0.09f:0.05f,selected?0.03f:0.06f,0.92f);
    draw_solid(x,y,w,3,selected?1.0f:(danger?0.72f:0.36f),selected?(danger?0.35f:0.62f):(danger?0.24f:0.36f),selected?0.08f:(danger?0.16f:0.42f),1.0f);
    draw_front_text(label,x+20,y+13,20,selected?1.0f:0.88f,selected?0.68f:0.88f,selected?0.10f:0.86f,1.0f);
}

std::string ra_mask_password(const std::string& src){ return std::string(src.size(), '*'); }

void draw_input_box(float x,float y,float w,float h,const char* label,const std::string& value,bool selected,bool password=false){
    draw_front_text(label,x,y-28,18,0.80f,0.80f,0.78f,1.0f);
    draw_solid(x,y,w,h,0.02f,0.03f,0.05f,0.96f);
    draw_solid(x,y,w,3,selected?1.0f:0.34f,selected?0.60f:0.34f,selected?0.08f:0.40f,1.0f);
    std::string shown = value.empty()?std::string(ra_ui_text("type_here")):(password?ra_mask_password(value):value);
    draw_front_text(shown.c_str(),x+16,y+14,20,value.empty()?0.45f:0.92f,value.empty()?0.45f:0.92f,value.empty()?0.44f:0.90f,1.0f);
}

void ra_login_open(){
    g_ra_login_overlay.open=true;
    g_ra_login_overlay.focus=0;
    g_ra_login_overlay.submitted=false;
    if(g_ra_login_overlay.user.empty()) g_ra_login_overlay.user=mickey_ra_username();
    std::fill(g_ra_login_overlay.pass.begin(),g_ra_login_overlay.pass.end(),'\0');
    g_ra_login_overlay.pass.clear();
    ra_text_input_start();
}
void ra_login_close(){
    g_ra_login_overlay.open=false;
    g_ra_login_overlay.focus=0;
    g_ra_login_overlay.submitted=false;
    std::fill(g_ra_login_overlay.pass.begin(),g_ra_login_overlay.pass.end(),'\0');
    g_ra_login_overlay.pass.clear();
    ra_text_input_stop();
}
void ra_login_submit(){
    if(g_ra_login_overlay.user.empty()||g_ra_login_overlay.pass.empty()) return;
    g_ra_login_overlay.submitted=true;
    (void)mickey_ra_login_with_password(g_ra_login_overlay.user.c_str(),g_ra_login_overlay.pass.c_str());
}
int ra_login_hit(float mx,float my){
    if(in_rect(mx,my,645,458,630,54)) return 0;
    if(in_rect(mx,my,645,558,630,54)) return 1;
    if(in_rect(mx,my,645,674,280,50)) return 2;
    if(in_rect(mx,my,995,674,280,50)) return 3;
    return -1;
}
void ra_login_sync_after_idle(int &selected){
    if(g_ra_login_overlay.open && g_ra_login_overlay.submitted && mickey_ra_logged_in()){
        ra_login_close();
        selected=0;
    }
}
bool ra_login_overlay_handle_event(const SDL_Event& e,int &selected,bool &acted){
    if(!g_ra_login_overlay.open) return false;
#if defined(PSX_SDL3)
    if(e.type==SDL_EVENT_MOUSE_MOTION){float vx=0,vy=0;mouse_virtual(e.motion.x,e.motion.y,vx,vy);int h=ra_login_hit(vx,vy);if(h>=0)g_ra_login_overlay.focus=h;return true;}
    if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button==SDL_BUTTON_LEFT){float vx=0,vy=0;mouse_virtual(e.button.x,e.button.y,vx,vy);int h=ra_login_hit(vx,vy);if(h>=0){g_ra_login_overlay.focus=h; if(h==2&&!mickey_ra_login_pending())ra_login_submit(); else if(h==3&&!mickey_ra_login_pending())ra_login_close();} return true;}
    if(e.type==SDL_EVENT_TEXT_INPUT){if(!mickey_ra_login_pending()){std::string* dst=(g_ra_login_overlay.focus==1)?&g_ra_login_overlay.pass:&g_ra_login_overlay.user; if(g_ra_login_overlay.focus<=1 && dst->size()<48) dst->append(e.text.text);} return true;}
    if(e.type==SDL_EVENT_KEY_DOWN && !e.key.repeat){SDL_Keycode key=e.key.key;
#else
    if(e.type==SDL_MOUSEMOTION){float vx=0,vy=0;mouse_virtual((float)e.motion.x,(float)e.motion.y,vx,vy);int h=ra_login_hit(vx,vy);if(h>=0)g_ra_login_overlay.focus=h;return true;}
    if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT){float vx=0,vy=0;mouse_virtual((float)e.button.x,(float)e.button.y,vx,vy);int h=ra_login_hit(vx,vy);if(h>=0){g_ra_login_overlay.focus=h; if(h==2&&!mickey_ra_login_pending())ra_login_submit(); else if(h==3&&!mickey_ra_login_pending())ra_login_close();} return true;}
    if(e.type==SDL_TEXTINPUT){if(!mickey_ra_login_pending()){std::string* dst=(g_ra_login_overlay.focus==1)?&g_ra_login_overlay.pass:&g_ra_login_overlay.user; if(g_ra_login_overlay.focus<=1 && dst->size()<48) dst->append(e.text.text);} return true;}
    if(e.type==SDL_KEYDOWN && !e.key.repeat){SDL_Keycode key=e.key.keysym.sym;
#endif
        if(key==SDLK_ESCAPE){if(!mickey_ra_login_pending())ra_login_close(); return true;}
        if(key==SDLK_TAB || key==SDLK_DOWN){g_ra_login_overlay.focus=(g_ra_login_overlay.focus+1)%4; menu_audio_pickup(); return true;}
        if(key==SDLK_UP){g_ra_login_overlay.focus=(g_ra_login_overlay.focus+3)%4; menu_audio_pickup(); return true;}
        if(key==SDLK_LEFT && g_ra_login_overlay.focus>=2){g_ra_login_overlay.focus=2; menu_audio_pickup(); return true;}
        if(key==SDLK_RIGHT && g_ra_login_overlay.focus>=2){g_ra_login_overlay.focus=3; menu_audio_pickup(); return true;}
        if(key==SDLK_BACKSPACE && !mickey_ra_login_pending()){
            std::string* dst=(g_ra_login_overlay.focus==1)?&g_ra_login_overlay.pass:&g_ra_login_overlay.user;
            if(g_ra_login_overlay.focus<=1 && !dst->empty()) dst->pop_back();
            return true;
        }
        if(key==SDLK_RETURN || key==SDLK_KP_ENTER){
            if(g_ra_login_overlay.focus==0){g_ra_login_overlay.focus=1; menu_audio_pickup();}
            else if(g_ra_login_overlay.focus==1 || g_ra_login_overlay.focus==2){ if(!mickey_ra_login_pending()) { menu_audio_enter(); ra_login_submit(); } }
            else if(g_ra_login_overlay.focus==3 && !mickey_ra_login_pending()){ menu_audio_enter(); ra_login_close(); }
            return true;
        }
        return true;
    }
    return true;
}

void render_ra_login_overlay(){
    draw_solid(0,0,VW,VH,0,0,0,0.62f);
    draw_frame_panel(580,295,760,470,0.98f,true);
    draw_front_text(ra_ui_text("login_title"),640,332,29,1.0f,0.60f,0.08f,1.0f);
    draw_front_text(ra_ui_text("login_sub"),640,370,18,0.78f,0.78f,0.76f,1.0f);
    draw_input_box(645,458,630,54,ra_ui_text("user"),g_ra_login_overlay.user,g_ra_login_overlay.focus==0,false);
    draw_input_box(645,558,630,54,ra_ui_text("pass"),g_ra_login_overlay.pass,g_ra_login_overlay.focus==1,true);
    draw_front_text(ra_ui_text("secret"),646,628,16,0.66f,0.66f,0.64f,1.0f);
    const bool pending = mickey_ra_login_pending()!=0;
    draw_action_button(645,674,280,50,ra_ui_text("submit"),g_ra_login_overlay.focus==2 && !pending,false);
    draw_action_button(995,674,280,50,ra_ui_text("cancel"),g_ra_login_overlay.focus==3 && !pending,true);
    const char* msg = pending ? mickey_ra_status() : (g_ra_login_overlay.submitted ? mickey_ra_status() : ra_ui_text("status_idle"));
    draw_front_text(msg,645,742,18,pending?0.84f:0.72f,pending?0.84f:0.72f,pending?0.82f:0.70f,1.0f);
}

void change_option(int row,int delta);
int ach_grid_cols();
int ach_grid_page_size();
#include "mickey_input_ui.inc"

void refresh_profile_thumbs() {
    for (int i=0;i<3;i++) {
        if (profile_thumb_tex[i].id) {
            glDeleteTextures(1,&profile_thumb_tex[i].id);
            profile_thumb_tex[i]=Tex{};
        }
        char n[32];
        std::snprintf(n,sizeof(n),"slot%d.bmp",i+1);
        fs::path p=exe_dir()/"profiles"/n;
        std::error_code ec;
        if (fs::is_regular_file(p,ec))
            profile_thumb_tex[i]=load_tex(p);
    }
}

void render_profile_page(Page page,int selected) {
    render_common();
    draw_solid(32,272,1856,735,0.005f,0.008f,0.016f,0.78f);
    draw_solid(32,272,1856,3,1.0f,0.47f,0.04f,0.90f);

    draw_front_text(page==Page::ProfileNew?tr("profile_new"):tr("profile_continue"),76,294,34,1.0f,0.60f,0.10f,1);

    const float card_y=390.0f, card_w=545.0f, card_h=500.0f;
    const float xs[3]={92.0f,687.0f,1282.0f};

    for (int i=0;i<3;i++) {
        MickeyProfileInfo info{};
        mickey_profiles_get_info(i,&info);
        const bool sel=i==selected;
        const float x=xs[i];

        draw_solid(x,card_y,card_w,card_h,0.012f,0.014f,0.020f,sel?0.94f:0.82f);
        const float br=sel?1.0f:0.42f;
        const float bg=sel?0.50f:0.42f;
        const float bb=sel?0.05f:0.42f;
        draw_solid(x,card_y,card_w,4,br,bg,bb,1);
        draw_solid(x,card_y+card_h-4,card_w,4,br,bg,bb,1);
        draw_solid(x,card_y,4,card_h,br,bg,bb,1);
        draw_solid(x+card_w-4,card_y,4,card_h,br,bg,bb,1);

        char line[96];
        std::snprintf(line,sizeof(line),"SLOT %d",i+1);
        draw_front_text(line,x+28,card_y+22,38,
                        sel?1.0f:0.94f,sel?0.72f:0.94f,sel?0.12f:0.92f,1);

        if (!info.exists) {
            draw_solid(x+45,card_y+100,455,255,0.025f,0.025f,0.030f,1.0f);
            draw_front_text(tr("empty"),x+122,card_y+205,35,0.78f,0.78f,0.76f,1);
            if (page==Page::ProfileNew)
                draw_front_text(tr("main_new"),x+158,card_y+396,29,0.92f,0.92f,0.90f,1);
            else
                draw_front_text(tr("no_save"),x+136,card_y+396,29,0.62f,0.62f,0.60f,1);
            continue;
        }

        if (profile_thumb_tex[i].id)
            draw_tex_rect(profile_thumb_tex[i],x+76,card_y+82,393,295,1,1,1,1);
        else
            draw_solid(x+76,card_y+82,393,295,0.025f,0.025f,0.030f,1.0f);

        draw_front_text(info.world_name,x+27,card_y+390,24,0.98f,0.86f,0.44f,1);
        std::snprintf(line,sizeof(line),"%s: %d",tr("lives"),info.lives);
        draw_front_text(line,x+27,card_y+426,22,0.92f,0.92f,0.90f,1);
        std::snprintf(line,sizeof(line),"%s: %d",tr("marbles"),info.marbles);
        draw_front_text(line,x+268,card_y+426,22,0.92f,0.92f,0.90f,1);
        draw_front_text(info.date_text,x+27,card_y+462,20,0.72f,0.72f,0.70f,1);

        if (page==Page::ProfileNew && sel)
            draw_front_text(tr("overwrite"),x+35,card_y+530,20,1.0f,0.66f,0.14f,1);
    }

    draw_front_text(mickey_input_legend(true),500,946,17,0.72f,0.72f,0.70f,1);
}

Tex &current_value_tex(int row) {
    if (row==0) return bezel_value_tex[std::clamp(g_cfg.bezel,0,4)];
    if (row==1) return crt_value_tex[std::clamp(g_cfg.crt_mode,0,5)];
    if (row==2) return res_value_tex[std::clamp(g_cfg.resolution,0,3)];
    return mode_value_tex[std::clamp(g_cfg.window_mode,0,2)];
}

void draw_preview() {
    const float px=835.0f, py=215.0f, pw=1010.0f, ph=568.0f;
    const float game_w=ph*(4.0f/3.0f);
    const float game_x=px+(pw-game_w)*0.5f;
    draw_solid(px-10,py-10,pw+20,ph+20,0,0,0,0.72f);
    if (g_cfg.bezel>0 && g_cfg.bezel<5 && bezel_preview_tex[g_cfg.bezel].id)
        draw_tex_rect(bezel_preview_tex[g_cfg.bezel],px,py,pw,ph,1,1,1,1);
    else
        draw_solid(px,py,pw,ph,0.025f,0.025f,0.03f,1.0f);
    draw_tex_rect_crt(preview_test_tex,game_x,py,game_w,ph,g_cfg.crt_mode);
    draw_solid(game_x-3,py,3,ph,1.0f,0.48f,0.04f,0.75f);
    draw_solid(game_x+game_w,py,3,ph,1.0f,0.48f,0.04f,0.75f);
}

void render_display_settings(int selected) {
    render_common();
    draw_front_text(tr("settings_title"),94,276,42,1.0f,0.58f,0.09f,1);
    const float ys[7]={350,430,510,590,670,750,840};
    const char* lab[7]={tr("bezel"),tr("crt"),tr("resolution"),tr("window_mode"),tr("language"),mickey_controls_text("controls"),tr("back")};
    for(int i=0;i<7;i++){
        bool sel=i==selected;
        draw_front_text(lab[i],94,ys[i],26,sel?1.0f:0.90f,sel?0.62f:0.90f,sel?0.08f:0.88f,1);
        if(i<5){
            char v[128]{};
            if(i==0) std::snprintf(v,sizeof(v),"%s",g_cfg.bezel?std::to_string(g_cfg.bezel).c_str():tr("none"));
            else if(i==1){const char* x[6]={tr("crt_off"),"PVM",tr("crt_classic"),tr("crt_tv"),tr("crt_arcade"),tr("crt_strong")};std::snprintf(v,sizeof(v),"%s",x[std::clamp(g_cfg.crt_mode,0,5)]);}
            else if(i==2){const char* x[4]={"720P","900P","1080P",tr("desktop")};std::snprintf(v,sizeof(v),"%s",x[std::clamp(g_cfg.resolution,0,3)]);}
            else if(i==3){const char* x[3]={tr("windowed"),tr("borderless"),tr("fullscreen")};std::snprintf(v,sizeof(v),"%s",x[std::clamp(g_cfg.window_mode,0,2)]);}
            else std::snprintf(v,sizeof(v),"%s",language_name(g_cfg.language));
            draw_front_text(v,460,ys[i]+1,22,0.98f,0.82f,0.40f,1);
        } else if(i==5) {
            draw_front_text(mickey_input_device_name(),460,ys[i]+1,20,0.98f,0.82f,0.40f,1);
        }
    }
    draw_front_text(tr("preview"),850,126,33,0.95f,0.95f,0.92f,1);
    draw_preview();
    draw_front_text(mickey_input_legend(true),900,826,16,0.68f,0.68f,0.66f,1);
}


int ach_grid_cols(){ return 8; }
int ach_grid_rows(){ return 3; }
int ach_grid_page_size(){ return ach_grid_cols()*ach_grid_rows(); }
std::string ach_abbrev(const std::string& src){
    std::string out; bool take=true;
    for(size_t i=0;i<src.size();++i){ unsigned char c=(unsigned char)src[i]; if(std::isalpha(c)){ if(take) out.push_back((char)std::toupper(c)); take=false; if(out.size()>=2) break; } else if(std::isspace(c) || c=='-' || c=='\'' || c=='&') take=true; }
    if(out.empty()){ for(char c:src){ unsigned char u=(unsigned char)c; if(std::isalnum(u)){ out.push_back((char)std::toupper(u)); if(out.size()>=2) break; } }
    }
    if(out.empty()) out="?";
    return out;
}
/* MICKEY_RA_I18N_V1_4_1_UNICODE_ACH_TEXT_FIX
 * Preserve UTF-8. The old code converted every byte >= 0x80 to a space. */
static size_t ach_utf8_len(const std::string& s){
    size_t n=0;
    for(unsigned char c : s)
        if((c & 0xC0u) != 0x80u) ++n;
    return n;
}
std::string ach_clean_text(const std::string& src){
    std::string out;
    out.reserve(src.size());
    bool last_space=true;

    for(size_t i=0;i<src.size();++i){
        const unsigned char c=(unsigned char)src[i];

        if(c>=0x80u){
            out.push_back((char)c);
            last_space=false;
            continue;
        }

        if(c==' ' || c=='\t' || c=='\r' || c=='\n'){
            if(!last_space){
                out.push_back(' ');
                last_space=true;
            }
            continue;
        }

        if(c<0x20u || c==0x7Fu)
            continue;

        out.push_back((char)c);
        last_space=false;
    }

    while(!out.empty() && out.front()==' ') out.erase(out.begin());
    while(!out.empty() && out.back()==' ') out.pop_back();
    return out;
}
std::vector<std::string> ach_wrap_lines(const std::string& src, size_t max_chars, int max_lines){
    std::vector<std::string> lines;
    const std::string clean=ach_clean_text(src);
    std::istringstream iss(clean);
    std::string word,line;

    while(iss >> word){
        const size_t word_len=ach_utf8_len(word);
        const size_t line_len=ach_utf8_len(line);

        if(line.empty())
            line=word;
        else if(line_len + 1 + word_len <= max_chars)
            line += " " + word;
        else {
            lines.push_back(line);
            line=word;
            if((int)lines.size() >= max_lines)
                break;
        }
    }

    if((int)lines.size() < max_lines && !line.empty())
        lines.push_back(line);
    if(lines.empty() && !clean.empty())
        lines.push_back(clean);
    if((int)lines.size() > max_lines)
        lines.resize(max_lines);
    return lines;
}

void draw_front_text_lines(const std::vector<std::string>& lines,float x,float y,float height,float line_gap,
                           float r,float g,float b,float a){
    for(size_t i=0;i<lines.size();++i) draw_front_text(lines[i].c_str(),x,y + (float)i*line_gap,height,r,g,b,a);
}
std::string ra_badge_key(const std::string& src){
    std::string out; out.reserve(src.size());
    for(unsigned char c:src){
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')) out.push_back((char)std::tolower(c));
        else if(c==' '||c=='-'||c=='_'||c=='\''||c=='!'||c=='?'||c=='&'||c==',') out.push_back(' ');
    }
    std::string clean; bool last_space=true;
    for(char c:out){ if(c==' '){ if(!last_space){ clean.push_back(' '); last_space=true; } } else { clean.push_back(c); last_space=false; } }
    while(!clean.empty() && clean.front()==' ') clean.erase(clean.begin());
    while(!clean.empty() && clean.back()==' ') clean.pop_back();
    return clean;
}
void ra_load_badge_manifest(){
    if(g_ra_badges_manifest_loaded) return;
    g_ra_badges_manifest_loaded=true;
    fs::path manifest = find_asset("ra_badges/manifest.txt");
    if(manifest.empty()) return;
    std::ifstream in(manifest, std::ios::binary);
    std::string line;
    while(std::getline(in,line)){
        if(line.empty()) continue;
        size_t p=line.find('|');
        if(p==std::string::npos) continue;
        std::string title=line.substr(0,p), file=line.substr(p+1);
        if(!title.empty()&&!file.empty()) g_ra_badge_manifest[ra_badge_key(title)] = manifest.parent_path()/file;
    }
}
#ifdef _WIN32
std::wstring ra_widen(const std::string& s){ if(s.empty())return{}; int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0); if(n<=0)return{}; std::wstring w((size_t)n,L'\0'); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,w.data(),n); if(!w.empty()&&w.back()==0)w.pop_back(); return w; }
bool ra_download_url(const std::string& url,const fs::path& file){
    if(url.empty())return false;
    using Fn=HRESULT (WINAPI*)(void*,LPCWSTR,LPCWSTR,DWORD,void*);
    static HMODULE dll=LoadLibraryW(L"urlmon.dll");
    static Fn fn=dll?reinterpret_cast<Fn>(GetProcAddress(dll,"URLDownloadToFileW")):nullptr;
    if(!fn)return false;
    std::error_code ec; fs::create_directories(file.parent_path(),ec);
    std::wstring wu=ra_widen(url), wf=file.wstring();
    if(wu.empty()||wf.empty())return false;
    return SUCCEEDED(fn(nullptr,wu.c_str(),wf.c_str(),0,nullptr)) && fs::is_regular_file(file,ec);
}
#else
bool ra_download_url(const std::string& url,const fs::path& file){
    auto response=mickey_http_request(url.c_str());
    if(response.status!=200||response.body.empty())return false;
    std::error_code ec; fs::create_directories(file.parent_path(),ec);
    const fs::path temporary=file.string()+".tmp";
    std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
    out.write(response.body.data(),(std::streamsize)response.body.size());
    out.close();
    if(!out){fs::remove(temporary,ec);return false;}
    fs::rename(temporary,file,ec);
    if(ec){fs::remove(temporary,ec);return false;}
    return true;
}
#endif
Tex* ra_badge_texture(const std::string& key,const std::string& url,const std::string& legacy_title){
    auto it=g_ra_badge_cache.find(key); if(it!=g_ra_badge_cache.end()) return it->second.id?&it->second:nullptr;
    ra_load_badge_manifest();
    fs::path file;
    auto legacy=g_ra_badge_manifest.find(ra_badge_key(legacy_title));
    if(legacy!=g_ra_badge_manifest.end()) file=legacy->second;
    if(file.empty()) file=exe_dir()/"ra_cache"/"badges"/(key+".png");
    std::error_code ec;
    if(!fs::is_regular_file(file,ec) && !url.empty() && !g_ra_badge_attempted[key]){
        const uint64_t now=SDL_GetTicks();
        if(now-g_ra_badge_last_download_tick>100){
            g_ra_badge_attempted[key]=true; g_ra_badge_last_download_tick=now;
            (void)ra_download_url(url,file);
        }
    }
    if(fs::is_regular_file(file,ec)){
        Tex t=load_tex(file); g_ra_badge_cache[key]=t; return g_ra_badge_cache[key].id?&g_ra_badge_cache[key]:nullptr;
    }
    return nullptr;
}
Tex* ra_badge_texture_for_achievement(const MickeyRAAchievement& a){
    const std::string url=(a.unlocked||!a.badge_locked_url[0])?std::string(a.badge_url):std::string(a.badge_locked_url);
    return ra_badge_texture(std::to_string(a.id)+(a.unlocked?"_u":"_l"),url,a.title);
}
Tex* ra_badge_texture_for_unlock(const MickeyRAUnlockEvent& e){
    return ra_badge_texture(std::to_string(e.id)+"_u",e.badge_url,e.title);
}
void ra_queue_unlock_toast(const MickeyRAUnlockEvent& e){
    RAUnlockToast t; t.active=false; t.start=0; t.id=e.id; t.title=e.title; t.desc=e.description; t.badge_url=e.badge_url; t.unlocked=true;
    g_ra_toast_queue.push_back(t);
}
void ra_poll_unlock_notifications(){
    MickeyRAUnlockEvent ev{};
    while(mickey_ra_pop_unlock_event(&ev)) ra_queue_unlock_toast(ev);
    if(!g_ra_toast_current.active && !g_ra_toast_queue.empty()){
        g_ra_toast_current=g_ra_toast_queue.front(); g_ra_toast_queue.erase(g_ra_toast_queue.begin());
        g_ra_toast_current.active=true; g_ra_toast_current.start=SDL_GetTicks();
        mickey_ui_play_sfx(L"Whoopie!.wav");
    }
    if(g_ra_toast_current.active){
        const uint64_t now=SDL_GetTicks();
        if(now-g_ra_toast_current.start>4600){
            g_ra_toast_current.active=false;
            if(!g_ra_toast_queue.empty()){
                g_ra_toast_current=g_ra_toast_queue.front(); g_ra_toast_queue.erase(g_ra_toast_queue.begin());
                g_ra_toast_current.active=true; g_ra_toast_current.start=now; mickey_ui_play_sfx(L"Whoopie!.wav");
            }
        }
    }
}
void ra_draw_unlock_toast(){
    if(!g_ra_toast_current.active) return;
    const uint64_t now=SDL_GetTicks();
    float t=(float)(now-g_ra_toast_current.start)/1000.0f;
    const float show=0.34f, hold=3.6f, hide=0.38f;
    float anim=1.0f;
    if(t<show) anim=ease(t/show);
    else if(t<show+hold) anim=1.0f;
    else anim=1.0f-ease(std::min(1.0f,(t-show-hold)/hide));
    const float w=560.0f,h=118.0f;
    const float x=VW-w-38.0f;
    const float y=-h + anim*(h+24.0f);
    draw_solid(x,y,w,h,0.01f,0.02f,0.04f,0.93f);
    draw_solid(x,y,w,3,1.0f,0.48f,0.05f,1.0f);
    draw_solid(x,y,3,h,1.0f,0.48f,0.05f,1.0f);
    draw_solid(x+16,y+18,82,82,0.12f,0.16f,0.20f,1.0f);
    MickeyRAUnlockEvent toast_ev{}; toast_ev.id=g_ra_toast_current.id; std::snprintf(toast_ev.title,sizeof(toast_ev.title),"%s",g_ra_toast_current.title.c_str()); std::snprintf(toast_ev.badge_url,sizeof(toast_ev.badge_url),"%s",g_ra_toast_current.badge_url.c_str());
    Tex* icon=ra_badge_texture_for_unlock(toast_ev);
    if(icon&&icon->id) draw_tex_rect(*icon,x+20,y+22,74,74,1,1,1,1);
    else { std::string ab=ach_abbrev(g_ra_toast_current.title); draw_front_text(ab.c_str(),x+28,y+40,28,1.0f,0.82f,0.34f,1.0f); }
    draw_front_text("RETROACHIEVEMENT",x+114,y+20,16,0.76f,0.76f,0.74f,1.0f);
    std::string title=localize_ach_title(g_ra_toast_current.title.c_str());
    std::string desc=localize_ach_desc(g_ra_toast_current.desc.c_str());
    auto title_lines=ach_wrap_lines(title, 28, 1);
    auto desc_lines=ach_wrap_lines(desc, 48, 2);
    draw_front_text_lines(title_lines,x+114,y+42,22,24,1.0f,0.84f,0.30f,1.0f);
    draw_front_text_lines(desc_lines,x+114,y+72,15,18,0.88f,0.88f,0.86f,1.0f);
}
void draw_achievement_tile(float x,float y,float w,float h,const MickeyRAAchievement& a,bool selected){
    const bool unlocked=a.unlocked!=0;
    draw_solid(x,y,w,h,0.02f,0.03f,0.05f,0.97f);
    draw_solid(x,y,w,3,selected?1.0f:(unlocked?0.48f:0.36f),selected?0.62f:(unlocked?0.72f:0.36f),selected?0.08f:(unlocked?0.18f:0.40f),1.0f);
    draw_solid(x,y+h-3,w,3,selected?1.0f:(unlocked?0.48f:0.28f),selected?0.62f:(unlocked?0.72f:0.28f),selected?0.08f:(unlocked?0.18f:0.34f),1.0f);
    draw_solid(x,y,3,h,selected?1.0f:(unlocked?0.48f:0.28f),selected?0.62f:(unlocked?0.72f:0.28f),selected?0.08f:(unlocked?0.18f:0.34f),1.0f);
    draw_solid(x+w-3,y,3,h,selected?1.0f:(unlocked?0.48f:0.28f),selected?0.62f:(unlocked?0.72f:0.28f),selected?0.08f:(unlocked?0.18f:0.34f),1.0f);
    draw_solid(x+8,y+8,w-16,h-28, unlocked?0.16f:0.08f, unlocked?0.26f:0.09f, unlocked?0.14f:0.12f, 0.95f);
    Tex* icon=ra_badge_texture_for_achievement(a);
    if(icon&&icon->id){
        const float tint=unlocked?1.0f:0.45f;
        draw_tex_rect(*icon,x+15,y+13,w-30,h-42,tint,tint,tint,1.0f);
    } else {
        std::string ab = unlocked ? ach_abbrev(localize_ach_title(a.title)) : std::string("?");
        draw_front_text(ab.c_str(),x+18,y+18,34,selected?1.0f:(unlocked?0.98f:0.72f),selected?0.66f:(unlocked?0.86f:0.72f),selected?0.10f:(unlocked?0.32f:0.72f),1.0f);
    }
    char pts[32]; std::snprintf(pts,sizeof(pts),"%u",a.points);
    draw_front_text(pts,x+12,y+h-19,14,0.88f,0.88f,0.86f,1.0f);
    draw_front_text("PTS",x+32,y+h-19,12,0.70f,0.70f,0.68f,1.0f);
    if(unlocked) draw_front_text("OK",x+w-36,y+h-21,15,0.32f,0.92f,0.42f,1.0f);
    else draw_front_text("LOCK",x+w-55,y+h-21,12,0.82f,0.82f,0.80f,1.0f);
}

void render_achievements(int selected) {
    render_common();
    mickey_ra_idle();
    draw_solid(24,244,1872,790,0.005f,0.008f,0.016f,0.82f);
    draw_solid(24,244,1872,3,1.0f,0.47f,0.04f,0.90f);
    draw_front_text(tr("ach_title"),74,266,34,1.0f,0.58f,0.09f,1);

    const int n=mickey_ra_achievement_count(), login=n, back=n+1;
    int unlocked=0,total_points=0,earned_points=0;
    for(int i=0;i<n;i++){MickeyRAAchievement a{}; if(!mickey_ra_get_achievement(i,&a)) continue; total_points+=(int)a.points; if(a.unlocked){unlocked++; earned_points+=(int)a.points;}}
    const int locked=std::max(0,n-unlocked);

    draw_frame_panel(60,324,420,590,0.84f,true);
    draw_front_text(ra_ui_text("account"),84,348,18,0.84f,0.84f,0.82f,1.0f);
    if(mickey_ra_logged_in()){
        auto user_lines=ach_wrap_lines(mickey_ra_username(), 20, 2);
        draw_front_text_lines(user_lines,84,378,24,26,1.0f,0.80f,0.34f,1.0f);
        draw_front_text("SOFTCORE MODE",84,440,16,0.88f,0.88f,0.86f,1.0f);
        auto mode_lines=ach_wrap_lines("Story uses checkpoints and savestates", 24, 2);
        draw_front_text_lines(mode_lines,84,463,14,17,0.68f,0.68f,0.66f,1.0f);
    } else {
        draw_front_text(tr("ra_not_logged"),84,380,20,0.96f,0.96f,0.94f,1.0f);
        auto need = ach_wrap_lines(ra_ui_text("need_login"), 24, 4);
        draw_front_text_lines(need,84,412,14,17,0.72f,0.72f,0.70f,1.0f);
    }

    draw_solid(84,510,372,2,1.0f,0.47f,0.04f,0.46f);
    draw_front_text(ra_ui_text("summary"),84,530,18,0.84f,0.84f,0.82f,1.0f);
    char s1[96]{},s2[96]{},s3[96]{};
    std::snprintf(s1,sizeof(s1),"%s  %d",ra_ui_text("unlocked"),unlocked);
    std::snprintf(s2,sizeof(s2),"%s  %d",ra_ui_text("locked"),locked);
    std::snprintf(s3,sizeof(s3),"PTS  %d / %d",earned_points,total_points);
    draw_front_text(s1,84,566,20,0.98f,0.82f,0.40f,1.0f);
    draw_front_text(s2,84,604,20,0.90f,0.90f,0.88f,1.0f);
    draw_front_text(s3,84,642,20,0.90f,0.90f,0.88f,1.0f);

    draw_solid(84,694,372,2,1.0f,0.47f,0.04f,0.30f);
    draw_front_text(ra_ui_text("preview_short"),84,714,18,0.84f,0.84f,0.82f,1.0f);
    std::string status_text = (mickey_ra_logged_in()||mickey_ra_login_pending())?mickey_ra_status():ra_ui_text("status_idle");
    auto status_lines = ach_wrap_lines(status_text, 24, 4);
    draw_front_text_lines(status_lines,84,746,14,17,0.70f,0.70f,0.68f,1.0f);
    auto hint_lines = ach_wrap_lines(mickey_input_legend(true), 40, 3);
    draw_front_text_lines(hint_lines,84,858,14,17,0.58f,0.58f,0.56f,1.0f);

    draw_frame_panel(510,324,1336,590,0.84f,false);
    if(n==0){
        g_ra_first_visible=0;
        auto none_lines = ach_wrap_lines(ra_ui_text("need_login"), 46, 3);
        draw_front_text_lines(none_lines,562,570,22,26,0.84f,0.84f,0.82f,1.0f);
    } else {
        const int cols=ach_grid_cols();
        const int rows=ach_grid_rows();
        const int page_size=ach_grid_page_size();
        const int first=(selected<login)?((selected/page_size)*page_size):g_ra_first_visible;
        g_ra_first_visible=std::min(first,std::max(0,n-1));
        const int page_count=(n + page_size - 1)/page_size;
        const int page_idx=(g_ra_first_visible/page_size)+1;
        MickeyRAAchievement cur{};
        const int preview_idx=(selected<n)?selected:std::min(g_ra_first_visible,n-1);
        mickey_ra_get_achievement(preview_idx,&cur);
        std::string title=localize_ach_title(cur.title);
        std::string desc=localize_ach_desc(cur.description);
        auto title_lines = ach_wrap_lines(title, 34, 2);
        auto desc_lines = ach_wrap_lines(desc, 62, 3);
        draw_front_text_lines(title_lines,552,348,22,26,cur.unlocked?1.0f:0.92f,cur.unlocked?0.66f:0.92f,cur.unlocked?0.10f:0.90f,1.0f);
        char meta[128]{};
        std::snprintf(meta,sizeof(meta),"%s • %u PTS • %s %d/%d", cur.unlocked?ra_ui_text("unlocked"):ra_ui_text("locked"), cur.points, ra_ui_text("page_short"), page_idx, page_count);
        draw_front_text(meta,552,404,15,0.72f,0.72f,0.70f,1.0f);
        draw_front_text_lines(desc_lines,552,430,16,20,0.78f,0.78f,0.76f,1.0f);

        const float start_x=552.0f, start_y=500.0f, tile_w=104.0f, tile_h=88.0f, step_x=150.0f, step_y=118.0f;
        for(int slot=0;slot<page_size;slot++){
            int idx=g_ra_first_visible+slot; if(idx>=n) break;
            MickeyRAAchievement a{}; if(!mickey_ra_get_achievement(idx,&a)) continue;
            int col=slot%cols, row=slot/cols;
            float x=start_x + col*step_x;
            float y=start_y + row*step_y;
            draw_achievement_tile(x,y,tile_w,tile_h,a,idx==selected);
        }
        char pager[64]{}; std::snprintf(pager,sizeof(pager),"%s %d/%d",ra_ui_text("page_short"),page_idx,page_count);
        draw_front_text(pager,1660,350,16,0.84f,0.84f,0.82f,1.0f);
    }

    const char* act = mickey_ra_logged_in() ? ra_ui_text("sign_out_short") : tr("ra_login");
    draw_action_button(82,946,300,52,act,selected==login,false);
    draw_action_button(1452,946,220,52,tr("back"),selected==back,true);

    if(g_ra_login_overlay.open) render_ra_login_overlay();
}

void present() {
    pBindVertexArray(0);
    pUseProgram(0);
    glBindTexture(GL_TEXTURE_2D,0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    SDL_GL_SwapWindow(g_window);
    /* Host menus remain responsive while guest VBlank heartbeats are stopped. */
    starvation_watchdog_heartbeat();
}

void render_page(Page page,int selected) {
    if (page==Page::Main) render_main(selected);
    else if (page==Page::ProfileNew || page==Page::ProfileContinue) render_profile_page(page,selected);
    else if (page==Page::Achievements) render_achievements(selected);
    else if (page==Page::Controls) render_controls_page(selected);
    else render_display_settings(selected);
}

void pump_quit_only() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
#if defined(PSX_SDL3)
        if (e.type==SDL_EVENT_QUIT) std::exit(0);
#else
        if (e.type==SDL_QUIT) std::exit(0);
#endif
    }
}

void circle_transition(Page page,int selected,bool closing,float duration=0.30f) {
    uint64_t start=SDL_GetTicks();
    float maxr=max_circle_radius();
    for (;;) {
        pump_quit_only();
        menu_audio_pump();
        uint64_t now=SDL_GetTicks();
        float t=(float)(now-start)/(duration*1000.0f);
        if (t>1.0f) t=1.0f;
        float e=ease(t);
        float r=closing ? maxr*(1.0f-e) : maxr*e;
        render_page(page,selected);
        draw_circle_mask(r);
        present();
        if (t>=1.0f) break;
        frontend_present_idle(1);
    }
}

void change_option(int row,int delta) {
    if (row==0) g_cfg.bezel=(g_cfg.bezel+delta+5)%5;
    else if (row==1) g_cfg.crt_mode=(g_cfg.crt_mode+delta+6)%6;
    else if (row==2) g_cfg.resolution=(g_cfg.resolution+delta+4)%4;
    else if (row==3) g_cfg.window_mode=(g_cfg.window_mode+delta+3)%3;
    else if (row==4) g_cfg.language=(g_cfg.language+delta+5)%5;
    save_config();
    if (row==2 || row==3) apply_window_settings();
}

int row_from_mouse(Page page,float mx,float my) {
    if (page==Page::Main) {
        const float ys[5]={372,472,572,672,772};
        for(int i=0;i<5;i++) if (mx>=75 && mx<=740 && my>=ys[i]-18 && my<=ys[i]+76) return i;
        return -1;
    }
    if (page==Page::ProfileNew || page==Page::ProfileContinue) {
        const float xs[3]={92.0f,687.0f,1282.0f};
        for(int i=0;i<3;i++) if (mx>=xs[i] && mx<=xs[i]+545 && my>=390 && my<=890) return i;
        return -1;
    }
    if(page==Page::Achievements) {
        if(g_ra_login_overlay.open) return -1;
        const int n=mickey_ra_achievement_count();
        const int cols=ach_grid_cols();
        const int rows=ach_grid_rows();
        const int first=g_ra_first_visible;
        const float start_x=552.0f, start_y=472.0f, tile_w=104.0f, tile_h=88.0f, step_x=150.0f, step_y=118.0f;
        for(int row=0;row<rows;row++) for(int col=0;col<cols;col++){ int slot=row*cols+col; int idx=first+slot; if(idx>=n) break; float x=start_x+col*step_x, y=start_y+row*step_y; if(mx>=x&&mx<=x+tile_w&&my>=y&&my<=y+tile_h) return idx; }
        if(mx>=82 && mx<=692 && my>=946 && my<=998) return n;
        if(mx>=1452 && mx<=1752 && my>=946 && my<=998) return n+1;
        return -1;
    }
    if(page==Page::Controls)
        return g_mickey_controls_rebinding ? -1 : mickey_controls_row_from_mouse(mx,my);
    const float ys[7]={350,430,510,590,670,750,840};
    for(int i=0;i<7;i++) if (my>=ys[i]-14 && my<=ys[i]+60) return i;
    return -1;
}

void mouse_virtual(float mx,float my,float &vx,float &vy) {
    int ww=1,wh=1;
    SDL_GetWindowSize(g_window,&ww,&wh);
    if (ww<1) ww=1; if (wh<1) wh=1;
    vx=mx*(VW/(float)ww);
    vy=my*(VH/(float)wh);
}

void start_game(Page page,int selected) {
    /* MICKEY_NEWGAME_TITLE_INPUT_GUARD_V1
     * Only NEW GAME should stop on the original title screen. */
    if (page==Page::ProfileNew) {
        g_mickey_newgame_title_input_guard=true;
        g_mickey_newgame_title_neutral_frames=0;
        std::fprintf(stdout,
            "MICKEY_NEWGAME_TITLE_INPUT_GUARD_V1: armed for original title screen\n");
        std::fflush(stdout);
    }

    /* MICKEY_INPUT_GAMEPLAY_HANDOFF_FIX_V1_3_FRONTEND */
    circle_transition(page,selected,true,0.64f);
    save_config();
    apply_game_settings();
    menu_audio_stop();

    /* Menu and gameplay use separate input consumers. Hand the physical pad
     * back before the GL init/present path returns to the PSX guest. */
    mickey_controls_release_gamepads_for_gameplay();

    g_game_open_active=true;
    g_game_open_start=0;
}

void switch_page(Page &page,int &selected,Page next,int next_selected=0) {
    circle_transition(page,selected,true);
    page=next;
    selected=next_selected;
    circle_transition(page,selected,false);
}
} // namespace

extern "C" void mickey_frontend_run(SDL_Window *window) {
    if (!window) return;

    const bool first_run=!g_ran;
    g_window=window;

    if (first_run) {
        g_ran=true;
        std::fprintf(stdout,
            "MICKEY_PAUSE_GAMEOVER_UIFIX_V3_ACTIVE: live CRT/bezel + same-window frontend return\n");
        std::fprintf(stdout,
            "MICKEY_FRONTEND_GAMEOVER_V2_ACTIVE: profiles + story Game Over recovery + PRACTICE no-save\n");
        std::fflush(stdout);

        if (!init_gl()) {
            std::fprintf(stderr,"mickey_frontend: OpenGL init failed; skipping frontend\n");
            return;
        }

        g_initialized=true;
        load_assets();
        const std::string profile_base=exe_dir().string();
        mickey_profiles_set_base_dir(profile_base.c_str());
        mickey_ra_init(profile_base.c_str());
        load_config();
        mickey_controls_init();
        apply_window_settings();
    } else {
        /* Pause -> Exit Game returns here without destroying/recreating the SDL
         * window or GL context. Assets and shaders already exist. */
        if (!g_initialized) return;
        load_config();
        mickey_controls_reload_pad_file();

        /* Gameplay keeps its own handle. Menu reacquires its temporary ref. */
        mickey_controls_reacquire_gamepads_for_frontend();

        apply_window_settings();
        std::fprintf(stdout,
            "MICKEY_UIFIX_V3_SAME_WINDOW_FRONTEND_REENTER\n");
        std::fflush(stdout);
    }

    refresh_profile_thumbs();

    Page page=Page::Main;
    int selected=0;

    if (first_run && !g_mickey_boot_intro_seen) {
        g_mickey_boot_intro_seen=true;
        run_startup_sequence(selected);
    } else {
        menu_audio_start();
        circle_transition(page,selected,false,0.45f);
    }

    bool running=true;
    while (running) {
        menu_audio_pump();
        mickey_ra_idle();
        ra_login_sync_after_idle(selected);
        render_page(page,selected);
        present();

        SDL_Event e;
        bool acted=false;
        bool delete_pressed=false;
        bool back_pressed=false;
        while (SDL_PollEvent(&e)) {
#if defined(PSX_SDL3)
            if (e.type==SDL_EVENT_QUIT) std::exit(0);
            if (page==Page::Achievements && ra_login_overlay_handle_event(e,selected,acted)) continue;
            if (e.type==SDL_EVENT_MOUSE_MOTION) {
                float vx=0,vy=0; mouse_virtual(e.motion.x,e.motion.y,vx,vy);
                int r=row_from_mouse(page,vx,vy);
                if (r>=0 && r!=selected) { selected=r; menu_audio_pickup(); }
            }
            if (e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button==SDL_BUTTON_LEFT) {
                mickey_input_mark_keyboard();
                float vx=0,vy=0; mouse_virtual(e.button.x,e.button.y,vx,vy);
                int r=row_from_mouse(page,vx,vy);
                if (r>=0) { selected=r; menu_audio_enter(); acted=true; }
            }
            if (e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                mickey_input_mark_gamepad();
                const std::string src=mickey_gamepad_button_source((int)e.gbutton.button);
                if(page==Page::Controls && g_mickey_controls_rebinding && g_mickey_rebind_kind==MickeyInputKind::Gamepad){mickey_controls_capture_gamepad(src);continue;}
                const MickeyInputAction a=mickey_action_from_pad_source(src);
                if(a!=MickeyInputAction::None){mickey_frontend_apply_action(page,selected,a,acted,back_pressed);continue;}
            }
            if (e.type==SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                const bool capture=page==Page::Controls && g_mickey_controls_rebinding && g_mickey_rebind_kind==MickeyInputKind::Gamepad;
                const std::string src=mickey_gamepad_axis_source((int)e.gaxis.axis,(int)e.gaxis.value,!capture);
                if(!src.empty()){mickey_input_mark_gamepad();if(capture){mickey_controls_capture_gamepad(src);continue;}const MickeyInputAction a=mickey_action_from_pad_source(src);if(a!=MickeyInputAction::None){mickey_frontend_apply_action(page,selected,a,acted,back_pressed);continue;}}
            }
            if (e.type==SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                SDL_Keycode key=e.key.key;
                const SDL_Scancode sc=e.key.scancode;
                if(page==Page::Controls && g_mickey_controls_rebinding && g_mickey_rebind_kind==MickeyInputKind::KeyboardMouse){mickey_input_mark_keyboard();mickey_controls_capture_keyboard(sc);continue;}
                mickey_input_mark_keyboard();
                const MickeyInputAction mapped=mickey_action_from_keyboard(sc,key);
                if(mapped!=MickeyInputAction::None){mickey_frontend_apply_action(page,selected,mapped,acted,back_pressed);continue;}
#else
            if (e.type==SDL_QUIT) std::exit(0);
            if (page==Page::Achievements && ra_login_overlay_handle_event(e,selected,acted)) continue;
            if (e.type==SDL_MOUSEMOTION) {
                float vx=0,vy=0; mouse_virtual((float)e.motion.x,(float)e.motion.y,vx,vy);
                int r=row_from_mouse(page,vx,vy);
                if (r>=0 && r!=selected) { selected=r; menu_audio_pickup(); }
            }
            if (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
                mickey_input_mark_keyboard();
                float vx=0,vy=0; mouse_virtual((float)e.button.x,(float)e.button.y,vx,vy);
                int r=row_from_mouse(page,vx,vy);
                if (r>=0) { selected=r; menu_audio_enter(); acted=true; }
            }
            if (e.type==SDL_CONTROLLERBUTTONDOWN) {
                mickey_input_mark_gamepad();
                const std::string src=mickey_gamepad_button_source((int)e.cbutton.button);
                if(page==Page::Controls && g_mickey_controls_rebinding && g_mickey_rebind_kind==MickeyInputKind::Gamepad){mickey_controls_capture_gamepad(src);continue;}
                const MickeyInputAction a=mickey_action_from_pad_source(src);
                if(a!=MickeyInputAction::None){mickey_frontend_apply_action(page,selected,a,acted,back_pressed);continue;}
            }
            if (e.type==SDL_CONTROLLERAXISMOTION) {
                const bool capture=page==Page::Controls && g_mickey_controls_rebinding && g_mickey_rebind_kind==MickeyInputKind::Gamepad;
                const std::string src=mickey_gamepad_axis_source((int)e.caxis.axis,(int)e.caxis.value,!capture);
                if(!src.empty()){mickey_input_mark_gamepad();if(capture){mickey_controls_capture_gamepad(src);continue;}const MickeyInputAction a=mickey_action_from_pad_source(src);if(a!=MickeyInputAction::None){mickey_frontend_apply_action(page,selected,a,acted,back_pressed);continue;}}
            }
            if (e.type==SDL_KEYDOWN && !e.key.repeat) {
                SDL_Keycode key=e.key.keysym.sym;
                const SDL_Scancode sc=e.key.keysym.scancode;
                if(page==Page::Controls && g_mickey_controls_rebinding && g_mickey_rebind_kind==MickeyInputKind::KeyboardMouse){mickey_input_mark_keyboard();mickey_controls_capture_keyboard(sc);continue;}
                mickey_input_mark_keyboard();
                const MickeyInputAction mapped=mickey_action_from_keyboard(sc,key);
                if(mapped!=MickeyInputAction::None){mickey_frontend_apply_action(page,selected,mapped,acted,back_pressed);continue;}
#endif
                int count=(page==Page::Main)?5:
                          ((page==Page::ProfileNew || page==Page::ProfileContinue)?3:
                          (page==Page::Achievements?mickey_ra_achievement_count()+2:
                          (page==Page::Controls?18:7)));

                if (page==Page::Achievements && (key==SDLK_UP || key==SDLK_DOWN || key==SDLK_LEFT || key==SDLK_RIGHT)) {
                    const int n=mickey_ra_achievement_count();
                    const int login=n, back=n+1;
                    const int cols=ach_grid_cols();
                    const int page_size=ach_grid_page_size();
                    int oldsel=selected;
                    if (selected < n) {
                        const int local = selected % page_size;
                        if (key==SDLK_LEFT) { if (selected>0) selected--; }
                        else if (key==SDLK_RIGHT) { if (selected+1<n) selected++; }
                        else if (key==SDLK_UP) { if (selected-cols>=0) selected-=cols; }
                        else if (key==SDLK_DOWN) { if (selected+cols<n) selected+=cols; else selected=login; }
                    } else {
                        if (key==SDLK_LEFT || key==SDLK_RIGHT) selected = (selected==login)?back:login;
                        else if (key==SDLK_UP) {
                            if (n>0) {
                                int last = std::min(n-1, g_ra_first_visible + page_size - 1);
                                selected = last;
                            }
                        }
                    }
                    if (selected!=oldsel) menu_audio_pickup();
                } else if (key==SDLK_UP || ((page==Page::ProfileNew || page==Page::ProfileContinue) && key==SDLK_LEFT)) {
                    selected=(selected+count-1)%count;
                    menu_audio_pickup();
                } else if (key==SDLK_DOWN || ((page==Page::ProfileNew || page==Page::ProfileContinue) && key==SDLK_RIGHT)) {
                    selected=(selected+1)%count;
                    menu_audio_pickup();
                } else if (page==Page::DisplaySettings && key==SDLK_LEFT && selected<5) {
                    change_option(selected,-1); menu_audio_pickup();
                } else if (page==Page::DisplaySettings && key==SDLK_RIGHT && selected<5) {
                    change_option(selected,+1); menu_audio_pickup();
                } else if ((page==Page::ProfileNew || page==Page::ProfileContinue) && key==SDLK_DELETE) {
                    delete_pressed=true;
                } else if (key==SDLK_RETURN || key==SDLK_KP_ENTER) {
                    menu_audio_enter(); acted=true;
                } else if (key==SDLK_ESCAPE) {
                    if (page==Page::DisplaySettings) {
                        switch_page(page,selected,Page::Main,3);
                    } else if (page==Page::Achievements) {
                        switch_page(page,selected,Page::Main,2);
                    } else if (page==Page::ProfileNew) {
                        switch_page(page,selected,Page::Main,0);
                    } else if (page==Page::ProfileContinue) {
                        switch_page(page,selected,Page::Main,1);
                    } else {
                        circle_transition(page,selected,true);
                        menu_audio_stop();
                        std::exit(0);
                    }
                }
            }
        }

        if (back_pressed) {
            if (page==Page::Controls) {g_mickey_controls_rebinding=false;g_mickey_controls_rebind_index=-1;switch_page(page,selected,Page::DisplaySettings,5);}
            else if (page==Page::DisplaySettings) switch_page(page,selected,Page::Main,3);
            else if (page==Page::Achievements) switch_page(page,selected,Page::Main,2);
            else if (page==Page::ProfileNew) switch_page(page,selected,Page::Main,0);
            else if (page==Page::ProfileContinue) switch_page(page,selected,Page::Main,1);
            else {circle_transition(page,selected,true);menu_audio_stop();std::exit(0);}
            frontend_present_idle(8);continue;
        }

        if (delete_pressed && (page==Page::ProfileNew || page==Page::ProfileContinue)) {
            menu_audio_enter();
            mickey_profiles_delete(selected);
            refresh_profile_thumbs();
        }

        if (acted) {
            if (page==Page::Main) {
                if (selected==0) switch_page(page,selected,Page::ProfileNew,0);
                else if (selected==1) switch_page(page,selected,Page::ProfileContinue,0);
                else if (selected==2) switch_page(page,selected,Page::Achievements,0);
                else if (selected==3) switch_page(page,selected,Page::DisplaySettings,0);
                else {
                    circle_transition(page,selected,true);
                    menu_audio_stop();
                    std::exit(0);
                }
            } else if (page==Page::ProfileNew) {
                mickey_profiles_select_new(selected);
                start_game(page,selected);
                running=false;
            } else if (page==Page::ProfileContinue) {
                if (mickey_profiles_select_continue(selected)) {
                    start_game(page,selected);
                    running=false;
                } else {
                    menu_audio_pickup();
                }
            } else if (page==Page::Achievements) {
                const int n=mickey_ra_achievement_count();
                if(selected==n){if(mickey_ra_logged_in()){mickey_ra_logout();selected=0;}else{ra_login_open();}}
                else if(selected==n+1)switch_page(page,selected,Page::Main,2);
            } else if (page==Page::DisplaySettings) {
                if (selected<5) change_option(selected,+1);
                else if (selected==5) switch_page(page,selected,Page::Controls,0);
                else switch_page(page,selected,Page::Main,3);
            } else if (page==Page::Controls) {
                if (selected<16) mickey_controls_begin_rebind(selected);
                else if (selected==16) mickey_controls_reset_active();
                else switch_page(page,selected,Page::DisplaySettings,5);
            }
        }

        frontend_present_idle(8);
    }

    pBindVertexArray(0);
    pUseProgram(0);
    glBindTexture(GL_TEXTURE_2D,0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
}

extern "C" int mickey_frontend_initialized(void) {
    return g_initialized ? 1 : 0;
}

/* MICKEY_PAUSE_GAMEOVER_UIFIX_V2_SHARED_HELPERS_BEGIN */
static void mickey_ui_flip_rgba_rows(std::vector<unsigned char> &rgba,
                                      int w, int h)
{
    if (w<=0 || h<=1)
        return;
    const size_t row=(size_t)w*4u;
    if (rgba.size() < row*(size_t)h)
        return;
    for (int y=0; y<h/2; ++y) {
        const size_t a=(size_t)y*row;
        const size_t b=(size_t)(h-1-y)*row;
        for (size_t x=0; x<row; ++x) {
            const unsigned char t=rgba[a+x];
            rgba[a+x]=rgba[b+x];
            rgba[b+x]=t;
        }
    }
}

static void mickey_ui_play_sfx(const wchar_t *filename)
{
#if defined(_WIN32)
    using PlaySoundWFn = BOOL (WINAPI *)(LPCWSTR, HMODULE, DWORD);
    static HMODULE winmm = LoadLibraryW(L"winmm.dll");
    static PlaySoundWFn play = winmm
        ? reinterpret_cast<PlaySoundWFn>(GetProcAddress(winmm,"PlaySoundW"))
        : nullptr;
    if (!play || !filename || !filename[0])
        return;

    wchar_t exe_buf[32768];
    const DWORD n=GetModuleFileNameW(nullptr,exe_buf,
        (DWORD)(sizeof(exe_buf)/sizeof(exe_buf[0])));
    if (n==0 || n >= (DWORD)(sizeof(exe_buf)/sizeof(exe_buf[0])))
        return;

    std::wstring path(exe_buf,exe_buf+n);
    const std::wstring::size_type slash=path.find_last_of(L"\\/");
    if (slash==std::wstring::npos)
        return;
    path.resize(slash+1);
    path += L"assets\\ui_sfx\\";
    path += filename;

    /* SND_ASYNC | SND_NODEFAULT | SND_FILENAME. Dynamic loading means no
     * additional winmm linker flag is needed. */
    play(path.c_str(),nullptr,0x00000001u|0x00000002u|0x00020000u);
#elif defined(PSX_SDL3)
    if (!filename || !filename[0]) return;
    const fs::path path=exe_dir()/"assets"/"ui_sfx"/filename;
    SDL_AudioSpec spec{};
    Uint8 *data=nullptr;
    Uint32 len=0;
    if (!SDL_LoadWAV(path.string().c_str(),&spec,&data,&len)) {
        std::fprintf(stderr,"mickey_frontend: UI sound %s: %s\n",
                     path.string().c_str(),SDL_GetError());
        return;
    }
    static SDL_AudioStream *stream=nullptr;
    if (!stream) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)!=0) {
            SDL_free(data);
            return;
        }
        stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                         &spec,nullptr,nullptr);
        if (!stream || !SDL_ResumeAudioStreamDevice(stream)) {
            std::fprintf(stderr,"mickey_frontend: UI audio: %s\n",SDL_GetError());
            if (stream) SDL_DestroyAudioStream(stream);
            stream=nullptr;
            SDL_free(data);
            return;
        }
    }
    SDL_ClearAudioStream(stream);
    if (!SDL_SetAudioStreamFormat(stream,&spec,nullptr) ||
        !SDL_PutAudioStreamData(stream,data,(int)len))
        std::fprintf(stderr,"mickey_frontend: UI audio: %s\n",SDL_GetError());
    SDL_free(data);
#else
    (void)filename;
#endif
}
/* MICKEY_PAUSE_GAMEOVER_UIFIX_V2_SHARED_HELPERS_END */

extern "C" int mickey_frontend_game_over_dialog(const char *world_name,
                                                  int initial_lives,
                                                  int can_restart,
                                                  int can_main_menu) {
    /* MICKEY_PAUSE_GAMEOVER_UIFIX_V2_GAMEOVER_ACTIVE */
    /* MICKEY_GAMEOVER_HYBRID_V4_2_UI_ACTIVE: animated/sound/orientation UI + v2.3 backend actions */
    if (!g_initialized || !g_window)
        return MICKEY_GAMEOVER_NONE;
    UiModalGuard modal(UiModalOwner::GameOver);
    if (!modal.acquired)
        return MICKEY_GAMEOVER_NONE;

    using FnBindFramebufferLocal = void (APIENTRY *)(GLenum, GLuint);
    using FnBlendFuncSeparateLocal = void (APIENTRY *)(GLenum, GLenum, GLenum, GLenum);
    using FnBlendEquationSeparateLocal = void (APIENTRY *)(GLenum, GLenum);

    auto bind_fb = reinterpret_cast<FnBindFramebufferLocal>(
        SDL_GL_GetProcAddress("glBindFramebuffer"));
    auto blend_func_sep = reinterpret_cast<FnBlendFuncSeparateLocal>(
        SDL_GL_GetProcAddress("glBlendFuncSeparate"));
    auto blend_eq_sep = reinterpret_cast<FnBlendEquationSeparateLocal>(
        SDL_GL_GetProcAddress("glBlendEquationSeparate"));

    if (!bind_fb) {
        std::fprintf(stderr,
            "mickey_frontend: Game Over modal requires glBindFramebuffer\n");
        return MICKEY_GAMEOVER_NONE;
    }

    constexpr GLenum K_DRAW_FRAMEBUFFER = 0x8CA9;
    constexpr GLenum K_READ_FRAMEBUFFER = 0x8CA8;
    constexpr GLenum K_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
    constexpr GLenum K_READ_FRAMEBUFFER_BINDING = 0x8CAA;
    constexpr GLenum K_CURRENT_PROGRAM = 0x8B8D;
    constexpr GLenum K_VERTEX_ARRAY_BINDING = 0x85B5;
    constexpr GLenum K_ACTIVE_TEXTURE = 0x84E0;
    constexpr GLenum K_TEXTURE0 = 0x84C0;
    constexpr GLenum K_BLEND_DST_RGB = 0x80C8;
    constexpr GLenum K_BLEND_SRC_RGB = 0x80C9;
    constexpr GLenum K_BLEND_DST_ALPHA = 0x80CA;
    constexpr GLenum K_BLEND_SRC_ALPHA = 0x80CB;
    constexpr GLenum K_BLEND_EQUATION_RGB = 0x8009;
    constexpr GLenum K_BLEND_EQUATION_ALPHA = 0x883D;

    struct SavedGL {
        GLint draw_fbo=0, read_fbo=0;
        GLint program=0, vao=0;
        GLint active_tex=0, tex0_binding=0;
        GLint viewport[4]{0,0,1,1};
        GLint scissor_box[4]{0,0,1,1};
        GLint read_buffer=GL_BACK;
        GLint pack_alignment=4;
        GLint blend_src_rgb=GL_ONE, blend_dst_rgb=GL_ZERO;
        GLint blend_src_alpha=GL_ONE, blend_dst_alpha=GL_ZERO;
        GLint blend_eq_rgb=GL_FUNC_ADD, blend_eq_alpha=GL_FUNC_ADD;
        GLboolean blend=GL_FALSE, scissor=GL_FALSE, depth=GL_FALSE, cull=GL_FALSE;
    } old{};

    glGetIntegerv(K_DRAW_FRAMEBUFFER_BINDING, &old.draw_fbo);
    glGetIntegerv(K_READ_FRAMEBUFFER_BINDING, &old.read_fbo);
    glGetIntegerv(K_CURRENT_PROGRAM, &old.program);
    glGetIntegerv(K_VERTEX_ARRAY_BINDING, &old.vao);
    glGetIntegerv(K_ACTIVE_TEXTURE, &old.active_tex);
    glGetIntegerv(GL_VIEWPORT, old.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, old.scissor_box);
    glGetIntegerv(GL_READ_BUFFER, &old.read_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old.pack_alignment);
    glGetIntegerv(K_BLEND_SRC_RGB, &old.blend_src_rgb);
    glGetIntegerv(K_BLEND_DST_RGB, &old.blend_dst_rgb);
    glGetIntegerv(K_BLEND_SRC_ALPHA, &old.blend_src_alpha);
    glGetIntegerv(K_BLEND_DST_ALPHA, &old.blend_dst_alpha);
    glGetIntegerv(K_BLEND_EQUATION_RGB, &old.blend_eq_rgb);
    glGetIntegerv(K_BLEND_EQUATION_ALPHA, &old.blend_eq_alpha);
    old.blend = glIsEnabled(GL_BLEND);
    old.scissor = glIsEnabled(GL_SCISSOR_TEST);
    old.depth = glIsEnabled(GL_DEPTH_TEST);
    old.cull = glIsEnabled(GL_CULL_FACE);

    pActiveTexture(K_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old.tex0_binding);
    pActiveTexture((GLenum)old.active_tex);

    bind_fb(K_READ_FRAMEBUFFER, 0);
    bind_fb(K_DRAW_FRAMEBUFFER, 0);

    Tex frozen{};
    {
        int fw=1, fh=1;
        SDL_GL_GetDrawableSize(g_window,&fw,&fh);
        if (fw<1) fw=1;
        if (fh<1) fh=1;
        std::vector<unsigned char> rgba((size_t)fw*(size_t)fh*4u);
        glReadBuffer(GL_FRONT);
        glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(0,0,fw,fh,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
        mickey_ui_flip_rgba_rows(rgba,fw,fh); /* MICKEY_UIFIX_V2_ORIENTATION_GAMEOVER */
        glReadBuffer(GL_BACK);

        glGenTextures(1,&frozen.id);
        frozen.w=fw;
        frozen.h=fh;
        pActiveTexture(K_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,frozen.id);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,fw,fh,0,
                     GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    }

    auto wait_for_accept_release = [&]() {
        const uint64_t end = SDL_GetTicks() + 450u;
        while (SDL_GetTicks() < end) {
            SDL_PumpEvents();
            const Uint8 *ks = SDL_GetKeyboardState(nullptr);
            const bool held = ks &&
                (ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_KP_ENTER] ||
                 ks[SDL_SCANCODE_SPACE]);
            if (!held) break;
            SDL_Delay(4);
        }
    };

    auto finish = [&](int action) -> int {
        wait_for_accept_release();

        pActiveTexture(K_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,0);
        if (frozen.id)
            glDeleteTextures(1,&frozen.id);
        frozen=Tex{};

        bind_fb(K_DRAW_FRAMEBUFFER, (GLuint)old.draw_fbo);
        bind_fb(K_READ_FRAMEBUFFER, (GLuint)old.read_fbo);
        pUseProgram((GLuint)old.program);
        pBindVertexArray((GLuint)old.vao);

        pActiveTexture(K_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,(GLuint)old.tex0_binding);
        pActiveTexture((GLenum)old.active_tex);

        glViewport(old.viewport[0],old.viewport[1],old.viewport[2],old.viewport[3]);
        glScissor(old.scissor_box[0],old.scissor_box[1],old.scissor_box[2],old.scissor_box[3]);

        if (old.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (old.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (old.depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (old.cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);

        if (blend_func_sep) {
            blend_func_sep((GLenum)old.blend_src_rgb,(GLenum)old.blend_dst_rgb,
                           (GLenum)old.blend_src_alpha,(GLenum)old.blend_dst_alpha);
        } else {
            glBlendFunc((GLenum)old.blend_src_rgb,(GLenum)old.blend_dst_rgb);
        }
        if (blend_eq_sep)
            blend_eq_sep((GLenum)old.blend_eq_rgb,(GLenum)old.blend_eq_alpha);

        glPixelStorei(GL_PACK_ALIGNMENT,old.pack_alignment);
        glReadBuffer((GLenum)old.read_buffer);
        glFlush();

        if (action == MICKEY_GAMEOVER_RESTART)
            g_gameover_deferred_load_black_hold = true;
        /* MICKEY_GAMEOVER_V44_MENU_POSTCLEANUP:
         * MENU must return to guest cleanup so it can naturally reach A5A0. */

        std::fprintf(stdout,
            "MICKEY_GAMEOVER_HYBRID_V4_3_PRESENT_SAFELOAD action=%d GL restored drawFBO=%d readFBO=%d program=%d vao=%d\n",
            action, old.draw_fbo, old.read_fbo, old.program, old.vao);
        std::fflush(stdout);
        return action;
    };

    PendingUiAction pending = PendingUiAction::None;
    int selected = can_restart ? 0 : 1;
    bool confirm_reset = false;
    int confirm_selected = 1;
    uint64_t confirm_opened_at = 0;
    uint64_t accept_after = 0;

    auto selectable = [&](int item) {
        return item == 0 ? (can_restart != 0) : (can_main_menu != 0);
    };
    auto move_selection = [&](int dir) {
        const int before=selected;
        for (int n=0; n<2; ++n) {
            selected = (selected + dir + 2) % 2;
            if (selectable(selected)) break;
        }
        if (selected!=before)
            mickey_ui_play_sfx(L"pickup.wav");
    };

    /* Small host-side entrance animation. It only animates the overlay shell;
     * the captured PS1 image never advances. */
    mickey_ui_play_sfx(L"window_open.wav");
    {
        const uint64_t t0=SDL_GetTicks();
        const uint64_t duration=165u;
        while (SDL_GetTicks()-t0 < duration) {
            const float t=(float)(SDL_GetTicks()-t0)/(float)duration;
            const float e=1.0f-(1.0f-t)*(1.0f-t)*(1.0f-t);
            bind_fb(K_DRAW_FRAMEBUFFER,0);
            bind_fb(K_READ_FRAMEBUFFER,0);
            int ww=1,wh=1;
            gl_setup_frame(&ww,&wh);
            if (frozen.id)
                draw_tex_rect(frozen,0,0,VW,VH,1,1,1,1);
            else
                draw_solid(0,0,VW,VH,0,0,0,1);
            draw_solid(0,0,VW,VH,0,0,0,0.42f*e);
            const float w=1110.0f*e;
            const float h=720.0f*e;
            const float x=960.0f-w*0.5f;
            const float y=540.0f-h*0.5f;
            draw_solid(x,y,w,h,0.004f,0.006f,0.012f,0.94f*e);
            draw_solid(x,y,w,5,1.0f,0.46f,0.03f,e);
            present();
            SDL_PumpEvents();
            frontend_present_idle(8);
        }
    }

    SDL_PumpEvents();
    const Uint8 *ks0=SDL_GetKeyboardState(nullptr);
    auto key_now = [&](SDL_Scancode sc)->bool { return ks0 && ks0[sc]!=0; };
    bool prev_up=key_now(SDL_SCANCODE_UP) || key_now(SDL_SCANCODE_LEFT);
    bool prev_down=key_now(SDL_SCANCODE_DOWN) || key_now(SDL_SCANCODE_RIGHT);
    bool prev_enter=key_now(SDL_SCANCODE_RETURN) || key_now(SDL_SCANCODE_KP_ENTER);
    bool prev_esc=key_now(SDL_SCANCODE_ESCAPE);
    accept_after=SDL_GetTicks()+120u;

    for (; pending == PendingUiAction::None;) {
        bind_fb(K_DRAW_FRAMEBUFFER,0);
        bind_fb(K_READ_FRAMEBUFFER,0);

        int ww=1,wh=1;
        gl_setup_frame(&ww,&wh);
        if (frozen.id)
            draw_tex_rect(frozen,0,0,VW,VH,1,1,1,1);
        else
            draw_solid(0,0,VW,VH,0,0,0,1);

        draw_solid(0,0,VW,VH,0.0f,0.0f,0.0f,0.42f);
        draw_solid(405,180,1110,720,0.004f,0.006f,0.012f,0.94f);
        draw_solid(405,180,1110,5,1.0f,0.46f,0.03f,1.0f);
        draw_front_text(tr("game_over"),650,235,76,1.0f,0.58f,0.09f,1.0f);
        draw_front_text(world_name ? world_name : tr("current_story"),
                        600,342,31,0.94f,0.94f,0.92f,1.0f);

        if (!confirm_reset) {
            const float ys[2]={505.0f,650.0f};
            const char *labels[2]={tr("restart_story"),tr("main_menu")};
            for (int i=0;i<2;i++) {
                const bool enabled=selectable(i);
                const bool sel=enabled && selected==i;
                draw_solid(520,ys[i]-24,880,92,
                           sel?0.12f:0.018f, sel?0.055f:0.020f,
                           sel?0.008f:0.026f, enabled?0.96f:0.46f);
                if (sel)
                    draw_solid(520,ys[i]-24,8,92,1.0f,0.48f,0.04f,1.0f);
                draw_front_text(labels[i],568,ys[i],38,
                                enabled?(sel?1.0f:0.88f):0.38f,
                                enabled?(sel?0.72f:0.88f):0.38f,
                                enabled?(sel?0.12f:0.86f):0.38f,1.0f);
            }
            char stats[96];
            std::snprintf(stats,sizeof(stats),tr("restart_stats"),initial_lives > 0 ? initial_lives : 3);
            draw_front_text(stats,655,790,25,0.80f,0.80f,0.78f,1.0f);
            draw_front_text(mickey_input_legend(true),650,844,17,0.56f,0.56f,0.54f,1.0f);
        } else {
            float ce=1.0f;
            if (confirm_opened_at) {
                const uint64_t elapsed=SDL_GetTicks()-confirm_opened_at;
                if (elapsed<135u) {
                    const float t=(float)elapsed/135.0f;
                    ce=1.0f-(1.0f-t)*(1.0f-t)*(1.0f-t);
                }
            }
            const float cw=900.0f*ce;
            const float ch=300.0f*ce;
            draw_solid(960.0f-cw*0.5f,605.0f-ch*0.5f,cw,ch,
                       0.012f,0.014f,0.020f,0.98f*ce);
            if (ce>0.82f) {
                draw_front_text(tr("confirm_story"),540,500,27,1.0f,0.72f,0.16f,1.0f);
                draw_front_text("",665,552,35,1.0f,0.72f,0.16f,1.0f);
                draw_front_text(tr("profile_reset"),
                                555,616,22,0.82f,0.82f,0.80f,1.0f);
                const char *yn[2]={tr("yes"),tr("no")};
                const float xs[2]={725.0f,1035.0f};
                for (int i=0;i<2;i++) {
                    const bool sel=confirm_selected==i;
                    draw_solid(xs[i]-38,675,190,70,
                               sel?0.14f:0.018f, sel?0.065f:0.020f,
                               sel?0.008f:0.026f,0.98f);
                    draw_front_text(yn[i],xs[i],690,34,
                                    sel?1.0f:0.88f, sel?0.72f:0.88f,
                                    sel?0.12f:0.86f,1.0f);
                }
            }
        }

        present();

        SDL_PumpEvents();
        const Uint8 *ks=SDL_GetKeyboardState(nullptr);
        auto kd = [&](SDL_Scancode sc)->bool { return ks && ks[sc]!=0; };
        const bool cur_up=kd(SDL_SCANCODE_UP) || kd(SDL_SCANCODE_LEFT) || kd(psx_keybinds_get_button(1,PSX_KB_UP)) || kd(psx_keybinds_get_button(1,PSX_KB_LEFT));
        const bool cur_down=kd(SDL_SCANCODE_DOWN) || kd(SDL_SCANCODE_RIGHT) || kd(psx_keybinds_get_button(1,PSX_KB_DOWN)) || kd(psx_keybinds_get_button(1,PSX_KB_RIGHT));
        const bool cur_enter=kd(SDL_SCANCODE_RETURN) || kd(SDL_SCANCODE_KP_ENTER) || kd(psx_keybinds_get_button(1,PSX_KB_CROSS));
        const bool cur_esc=kd(SDL_SCANCODE_ESCAPE) || kd(psx_keybinds_get_button(1,PSX_KB_CIRCLE));
        const bool press_up=cur_up && !prev_up;
        const bool press_down=cur_down && !prev_down;
        const bool press_enter=cur_enter && !prev_enter;
        const bool press_esc=cur_esc && !prev_esc;
        if(press_up || press_down || press_enter || press_esc) mickey_input_mark_keyboard();
        prev_up=cur_up; prev_down=cur_down; prev_enter=cur_enter; prev_esc=cur_esc;

        if (!confirm_reset) {
            if (press_up) move_selection(-1);
            if (press_down) move_selection(+1);
            if (press_enter && SDL_GetTicks() >= accept_after) {
                if (selected==0 && can_restart) {
                    mickey_ui_play_sfx(L"enter.wav");
                    pending=PendingUiAction::RestartGame;
                }
                if (selected==1 && can_main_menu) {
                    mickey_ui_play_sfx(L"enter.wav");
                    confirm_reset=true;
                    confirm_selected=1;
                    confirm_opened_at=SDL_GetTicks();
                    accept_after=confirm_opened_at+180u;
                }
            }
            if (press_esc && pending==PendingUiAction::None && can_main_menu) {
                selected=1;
                confirm_reset=true;
                confirm_selected=1;
                confirm_opened_at=SDL_GetTicks();
                accept_after=confirm_opened_at+180u;
                mickey_ui_play_sfx(L"back.wav");
            }
        } else {
            if (press_up || press_down) {
                confirm_selected=1-confirm_selected;
                mickey_ui_play_sfx(L"pickup.wav");
            }
            if (press_enter && SDL_GetTicks() >= accept_after) {
                if (confirm_selected==0) {
                    mickey_ui_play_sfx(L"enter.wav");
                    pending=PendingUiAction::ReturnToFrontend;
                } else {
                    confirm_reset=false;
                    accept_after=SDL_GetTicks()+180u;
                    mickey_ui_play_sfx(L"back.wav");
                }
            }
            if (press_esc && pending==PendingUiAction::None) {
                confirm_reset=false;
                accept_after=SDL_GetTicks()+180u;
                mickey_ui_play_sfx(L"back.wav");
            }
        }

        SDL_Event e;
        while (pending==PendingUiAction::None && SDL_PollEvent(&e)) {
#if defined(PSX_SDL3)
            if (e.type==SDL_EVENT_QUIT)
                pending=PendingUiAction::Quit;
            if (e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                mickey_input_mark_gamepad();
                const int b=(int)e.gbutton.button;
                const std::string msrc=mickey_gamepad_button_source(b);
                const MickeyInputAction mia=mickey_action_from_pad_source(msrc);
                const bool pad_up=mia==MickeyInputAction::Up;
                const bool pad_down=mia==MickeyInputAction::Down;
                const bool pad_left=mia==MickeyInputAction::Left;
                const bool pad_right=mia==MickeyInputAction::Right;
                const bool pad_confirm=mia==MickeyInputAction::Confirm;
                const bool pad_back=mia==MickeyInputAction::Back;
                const bool pad_start=mickey_pad_binding_contains(14,msrc);
                if (!confirm_reset) {
                    if (pad_up || pad_left)
                        move_selection(-1);
                    else if (pad_down || pad_right)
                        move_selection(+1);
                    else if (pad_confirm && SDL_GetTicks() >= accept_after) {
                        if (selected==0 && can_restart) {
                            mickey_ui_play_sfx(L"enter.wav");
                            pending=PendingUiAction::RestartGame;
                        }
                        if (selected==1 && can_main_menu) {
                            mickey_ui_play_sfx(L"enter.wav");
                            confirm_reset=true; confirm_selected=1;
                            confirm_opened_at=SDL_GetTicks();
                            accept_after=confirm_opened_at+180u;
                        }
                    } else if (pad_back && can_main_menu) {
                        selected=1; confirm_reset=true; confirm_selected=1;
                        confirm_opened_at=SDL_GetTicks();
                        accept_after=confirm_opened_at+180u;
                        mickey_ui_play_sfx(L"back.wav");
                    }
                } else {
                    if (pad_left || pad_right ||
                        pad_up || pad_down) {
                        confirm_selected=1-confirm_selected;
                        mickey_ui_play_sfx(L"pickup.wav");
                    } else if (pad_confirm && SDL_GetTicks() >= accept_after) {
                        if (confirm_selected==0) {
                            mickey_ui_play_sfx(L"enter.wav");
                            pending=PendingUiAction::ReturnToFrontend;
                        }
                        if (pending==PendingUiAction::None) {
                            confirm_reset=false;
                            accept_after=SDL_GetTicks()+180u;
                            mickey_ui_play_sfx(L"back.wav");
                        }
                    } else if (pad_back) {
                        confirm_reset=false;
                        accept_after=SDL_GetTicks()+180u;
                        mickey_ui_play_sfx(L"back.wav");
                    }
                }
            }
            if (e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button==SDL_BUTTON_LEFT) {
                float vx=0,vy=0; mouse_virtual(e.button.x,e.button.y,vx,vy);
#else
            if (e.type==SDL_QUIT)
                pending=PendingUiAction::Quit;
            if (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
                float vx=0,vy=0; mouse_virtual((float)e.button.x,(float)e.button.y,vx,vy);
#endif
                if (!confirm_reset) {
                    if (vx>=520 && vx<=1400 && vy>=481 && vy<=573 && can_restart && SDL_GetTicks()>=accept_after) {
                        selected=0;
                        mickey_ui_play_sfx(L"enter.wav");
                        pending=PendingUiAction::RestartGame;
                    }
                    if (vx>=520 && vx<=1400 && vy>=626 && vy<=718 && can_main_menu && SDL_GetTicks()>=accept_after) {
                        selected=1;
                        mickey_ui_play_sfx(L"enter.wav");
                        confirm_reset=true;
                        confirm_selected=1;
                        confirm_opened_at=SDL_GetTicks();
                        accept_after=confirm_opened_at+180u;
                    }
                } else {
                    if (vx>=687 && vx<=877 && vy>=675 && vy<=745 && SDL_GetTicks()>=accept_after) {
                        mickey_ui_play_sfx(L"enter.wav");
                        pending=PendingUiAction::ReturnToFrontend;
                    }
                    if (vx>=997 && vx<=1187 && vy>=675 && vy<=745) {
                        confirm_reset=false;
                        accept_after=SDL_GetTicks()+180u;
                        mickey_ui_play_sfx(L"back.wav");
                    }
                }
            }
        }
        if (pending==PendingUiAction::None) frontend_present_idle(8);
    }
    const int action = pending==PendingUiAction::RestartGame ? MICKEY_GAMEOVER_RESTART :
                       pending==PendingUiAction::ReturnToFrontend ? MICKEY_GAMEOVER_MAIN_MENU :
                       MICKEY_GAMEOVER_NONE;
    return finish(action);
}

/* ========================================================================
 * MICKEY_PAUSE_GAMEOVER_UIFIX_V3_PAUSE_ACTIVE
 *
 * Host-side pause menu. The guest is blocked while this loop runs, so no PS1
 * state advances. START is consumed by mickey_frontend_pause_filter_buttons()
 * before it reaches SIO, which prevents Mickey's original pause code from
 * opening underneath this overlay.
 * ======================================================================== */

static bool g_mickey_pause_requested = false;
static bool g_mickey_pause_start_held = false;
static bool g_mickey_pause_escape_held = false;
static bool g_mickey_frontend_return_black_hold = false;
/* Pause changes are committed by the host frame tick, after the complete
 * renderer present has unwound. */
static bool g_mickey_pause_reopen_options = false;
static int  g_mickey_pause_reopen_option_selected = 0;


static void mickey_pause_draw_iris_mask(float radius)
{
    const float cx=VW*0.5f;
    const float cy=VH*0.5f;
    const int strips=144;
    const float sw=(float)VW/(float)strips;
    if (radius < 0.0f) radius=0.0f;

    for (int i=0;i<strips;i++) {
        const float x0=i*sw;
        const float xm=x0+sw*0.5f;
        const float dx=xm-cx;
        if (radius<=0.5f || dx<=-radius || dx>=radius) {
            draw_solid(x0,0,sw+1.0f,VH,0,0,0,1);
            continue;
        }
        const float rr=radius*radius-dx*dx;
        const float yext=rr>0.0f ? (float)std::sqrt((double)rr) : 0.0f;
        const float top=cy-yext;
        const float bot=cy+yext;
        if (top>0.0f)
            draw_solid(x0,0,sw+1.0f,top+1.0f,0,0,0,1);
        if (bot<(float)VH)
            draw_solid(x0,bot,sw+1.0f,(float)VH-bot+1.0f,0,0,0,1);
    }
}

static void mickey_pause_restore_clean_frame(const Tex &frozen)
{
    int ww=1, wh=1;
    gl_setup_frame(&ww,&wh);
    if (frozen.id)
        draw_tex_rect(frozen,0,0,VW,VH,1,1,1,1);
    else
        draw_solid(0,0,VW,VH,0,0,0,1);

    pBindVertexArray(0);
    pUseProgram(0);
    glBindTexture(GL_TEXTURE_2D,0);
    glDisable(GL_BLEND);
}



static PendingUiAction mickey_pause_dialog()
{
    if (!g_initialized || !g_window)
        return PendingUiAction::None;
    UiModalGuard modal(UiModalOwner::Pause);
    if (!modal.acquired)
        return PendingUiAction::None;

    const bool reopen_options = g_mickey_pause_reopen_options;
    const int reopen_option_selected =
        std::clamp(g_mickey_pause_reopen_option_selected,0,3);
    g_mickey_pause_reopen_options = false;

    /* We are called from mickey_frontend_draw_game_transition(), immediately
     * before the renderer's SDL_GL_SwapWindow. GL_BACK therefore contains the
     * newest complete gameplay frame. Capture that exact frame and keep drawing
     * it while the guest is frozen. */
    Tex frozen{};
    int frozen_w=1, frozen_h=1;
    std::vector<unsigned char> frozen_rgba;
    {
        int fw=1,fh=1;
        SDL_GL_GetDrawableSize(g_window,&fw,&fh);
        if (fw<1) fw=1;
        if (fh<1) fh=1;
        frozen_w=fw;
        frozen_h=fh;
        frozen_rgba.resize((size_t)fw*(size_t)fh*4u);
        GLint old_read=GL_BACK;
        glGetIntegerv(GL_READ_BUFFER,&old_read);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(0,0,fw,fh,GL_RGBA,GL_UNSIGNED_BYTE,frozen_rgba.data());
        mickey_ui_flip_rgba_rows(frozen_rgba,fw,fh); /* MICKEY_UIFIX_V3_ORIENTATION_PAUSE */
        glReadBuffer((GLenum)old_read);
    }

    auto release_frozen=[&]() {
        glBindTexture(GL_TEXTURE_2D,0);
        if (frozen.id)
            glDeleteTextures(1,&frozen.id);
        frozen=Tex{};
    };

    auto upload_frozen=[&]() {
        if (frozen_rgba.empty() || frozen_w<1 || frozen_h<1)
            return;
        glGenTextures(1,&frozen.id);
        frozen.w=frozen_w;
        frozen.h=frozen_h;
        glBindTexture(GL_TEXTURE_2D,frozen.id);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,frozen_w,frozen_h,0,
                     GL_RGBA,GL_UNSIGNED_BYTE,frozen_rgba.data());
    };

    upload_frozen();

    /* Defined before apply_pause_option because the lambda changes the currently
     * highlighted display option. */
    int option_selected=reopen_options ? reopen_option_selected : 0;
    PendingUiAction pending=PendingUiAction::None;

    auto apply_pause_option=[&](int dir) {
        /* All four settings stage one action. The modal releases its frozen
         * frame before the host tick changes renderer or window resources. */
        if (option_selected==2) {
            g_cfg.resolution=(g_cfg.resolution+dir+4)%4;
            save_config();
            g_mickey_pause_reopen_option_selected=option_selected;
            g_mickey_pause_reopen_options=true;
            g_pending_display_window_change=true;
            pending=PendingUiAction::ApplyDisplaySettings;
            std::fprintf(stdout,
                "MICKEY_PAUSE_POST_SWAP_HANDOFF_V4: stage resolution=%d\n",
                g_cfg.resolution);
            std::fflush(stdout);
            return;
        }

        if (option_selected==3) {
            g_cfg.window_mode=(g_cfg.window_mode+dir+3)%3;
            save_config();
            g_mickey_pause_reopen_option_selected=option_selected;
            g_mickey_pause_reopen_options=true;
            g_pending_display_window_change=true;
            pending=PendingUiAction::ApplyDisplaySettings;
            std::fprintf(stdout,
                "MICKEY_PAUSE_POST_SWAP_HANDOFF_V4: stage window_mode=%d\n",
                g_cfg.window_mode);
            std::fflush(stdout);
            return;
        }

        /* The renderer owns bezel/CRT resources. Apply after the outer present
         * has returned; reopening Options keeps the visual interaction live. */
        change_option(option_selected,dir);
        g_pending_display_window_change=false;
        g_mickey_pause_reopen_option_selected=option_selected;
        g_mickey_pause_reopen_options=true;
        pending=PendingUiAction::ApplyDisplaySettings;
    };

    auto finish=[&](PendingUiAction action)->PendingUiAction {
        if (action==PendingUiAction::ReturnToFrontend ||
            action==PendingUiAction::Quit || g_mickey_frontend_return_black_hold) {
            int ww=1,wh=1;
            gl_setup_frame(&ww,&wh);
            draw_solid(0,0,VW,VH,0,0,0,1);
            pBindVertexArray(0);
            pUseProgram(0);
            glBindTexture(GL_TEXTURE_2D,0);
            glDisable(GL_BLEND);
        } else {
            /* Leave one clean gameplay frame in BACK. The outer renderer owns
             * the swap after this nested pause loop returns. */
            mickey_pause_restore_clean_frame(frozen);
        }
        release_frozen();
        SDL_PumpEvents();
        const Uint8 *ks=SDL_GetKeyboardState(nullptr);
        g_mickey_pause_escape_held =
            ks && ks[SDL_SCANCODE_ESCAPE]!=0;
        g_mickey_pause_requested=false;
        return action;
    };
    int selected=reopen_options ? 2 : 0;
    bool options_focus=reopen_options;
    bool confirm_restart=false;
    bool confirm_quit=false;
    int confirm_selected=1; /* default NIE */
    uint64_t error_until=0;
    const char *error_text=tr("cannot_checkpoint");
    const uint64_t opened_at=SDL_GetTicks();

    auto move_main=[&](int dir) {
        const int old=selected;
        selected=(selected+dir+4)%4;
        if (selected!=old)
            mickey_ui_play_sfx(L"pickup.wav");
    };

    auto draw_main=[&]() {
        const char *labels[4]={
            tr("resume"),
            tr("restart_level"),
            tr("options"),
            tr("exit_game")
        };
        const float ys[4]={350.0f,470.0f,590.0f,710.0f};
        const bool can_restart=mickey_profiles_pause_can_restart()!=0;

        draw_solid(78,92,655,895,0.004f,0.006f,0.012f,0.94f);
        draw_solid(78,92,655,5,1.0f,0.46f,0.03f,1.0f);
        draw_front_text(tr("pause"),148,148,72,1.0f,0.58f,0.09f,1.0f);

        const char *world=mickey_profiles_pause_world_name();
        if (world && world[0])
            draw_front_text(world,148,247,25,0.78f,0.78f,0.76f,1.0f);

        for (int i=0;i<4;i++) {
            const bool enabled=(i!=1) || can_restart;
            const bool sel=!options_focus && selected==i && enabled;
            draw_solid(126,ys[i]-20,555,82,
                       sel?0.12f:0.015f,
                       sel?0.055f:0.018f,
                       sel?0.008f:0.024f,
                       enabled?0.96f:0.48f);
            if (sel)
                draw_solid(126,ys[i]-20,7,82,1.0f,0.48f,0.04f,1.0f);
            draw_front_text(labels[i],168,ys[i],35,
                            enabled?(sel?1.0f:0.90f):0.42f,
                            enabled?(sel?0.72f:0.90f):0.42f,
                            enabled?(sel?0.12f:0.88f):0.42f,1.0f);
        }

        if (mickey_profiles_is_practice()) {
            draw_front_text(tr("practice_nosave"),
                            128,842,19,0.96f,0.62f,0.20f,1.0f);
        } else if (!can_restart) {
            draw_front_text(tr("checkpoint_not_ready"),
                            128,842,19,0.78f,0.70f,0.56f,1.0f);
        }

        draw_front_text(tr("esc_resume"),128,920,20,0.58f,0.58f,0.56f,1.0f);
    };

    auto draw_options=[&]() {
        if (!options_focus)
            return;

        draw_solid(790,132,1050,810,0.005f,0.008f,0.016f,0.95f);
        draw_solid(790,132,1050,5,1.0f,0.46f,0.03f,1.0f);
        draw_front_text(tr("options"),850,185,54,1.0f,0.58f,0.09f,1.0f);
        draw_front_text(tr("live_apply"),
                        850,258,22,0.70f,0.70f,0.68f,1.0f);

        /* MICKEY_PAUSE_REMOVE_LANGUAGE_V1_1 */
        const float ys[4]={338.0f,442.0f,546.0f,650.0f};
        const char* olab[4]={tr("bezel"),tr("crt"),tr("resolution"),tr("window_mode")};
        for (int i=0;i<4;i++) {
            const bool sel=option_selected==i; draw_solid(835,ys[i]-20,920,76,sel?0.10f:0.014f,sel?0.050f:0.018f,sel?0.008f:0.024f,0.96f); if(sel)draw_solid(835,ys[i]-20,7,76,1.0f,0.48f,0.04f,1.0f);
            draw_front_text(olab[i],875,ys[i],26,sel?1.0f:0.88f,sel?0.62f:0.88f,sel?0.08f:0.86f,1);
            char pv[160]{};
            if(i==0) std::snprintf(pv,sizeof(pv),"%s",g_cfg.bezel?std::to_string(g_cfg.bezel).c_str():tr("none"));
            else if(i==1){const char* x[6]={tr("crt_off"),"PVM",tr("crt_classic"),tr("crt_tv"),tr("crt_arcade"),tr("crt_strong")};std::snprintf(pv,sizeof(pv),"%s",x[std::clamp(g_cfg.crt_mode,0,5)]);}
            else if(i==2){const char* x[4]={"720P","900P","1080P",tr("desktop")};std::snprintf(pv,sizeof(pv),"%s",x[std::clamp(g_cfg.resolution,0,3)]);}
            else if(i==3){const char* x[3]={tr("windowed"),tr("borderless"),tr("fullscreen")};std::snprintf(pv,sizeof(pv),"%s",x[std::clamp(g_cfg.window_mode,0,2)]);}
            draw_front_text(pv,1320,ys[i]+1,25,0.98f,0.82f,0.40f,1);
        }

        draw_front_text(mickey_input_legend(true),
                        850,858,20,0.62f,0.62f,0.60f,1.0f);
        draw_front_text(tr("back_pause"),
                        850,895,20,0.62f,0.62f,0.60f,1.0f);
    };

    auto draw_confirm=[&]() {
        draw_solid(0,0,VW,VH,0,0,0,0.34f);
        draw_solid(400,310,1120,470,0.005f,0.008f,0.016f,0.985f);
        draw_solid(400,310,1120,5,1.0f,0.46f,0.03f,1.0f);

        if (confirm_quit) {
            draw_front_text(tr("confirm_exit"),
                            575,385,31,1.0f,0.70f,0.14f,1.0f);
            draw_front_text(tr("unsaved"),
                            500,462,25,0.90f,0.90f,0.87f,1.0f);
        } else {
            draw_front_text(tr("confirm_restart_level"),
                            500,385,31,1.0f,0.70f,0.14f,1.0f);
            draw_front_text(tr("unsaved"),
                            590,462,27,0.90f,0.90f,0.87f,1.0f);
        }

        const char *yn[2]={tr("yes"),tr("no")};
        const float xs[2]={690.0f,1070.0f};
        for (int i=0;i<2;i++) {
            const bool sel=confirm_selected==i;
            draw_solid(xs[i]-45,585,245,90,
                       sel?0.13f:0.018f,
                       sel?0.060f:0.020f,
                       sel?0.008f:0.026f,0.98f);
            draw_front_text(yn[i],xs[i]+20,605,39,
                            sel?1.0f:0.88f,
                            sel?0.72f:0.88f,
                            sel?0.12f:0.86f,1.0f);
        }
    };


    auto draw_pause_base=[&]() {
        int ww=1,wh=1;
        gl_setup_frame(&ww,&wh);
        if (frozen.id)
            draw_tex_rect(frozen,0,0,VW,VH,1,1,1,1);
        else
            draw_solid(0,0,VW,VH,0,0,0,1);
        draw_solid(0,0,VW,VH,0,0,0,0.48f);
        draw_main();
        draw_options();
        if (confirm_restart || confirm_quit)
            draw_confirm();
    };

    auto animate_box=[&](float x,float y,float w,float h) {
        const uint64_t t0=SDL_GetTicks();
        const uint64_t duration=150u;
        while (SDL_GetTicks()-t0 < duration) {
            const float t=(float)(SDL_GetTicks()-t0)/(float)duration;
            const float e=1.0f-(1.0f-t)*(1.0f-t)*(1.0f-t);
            int ww=1,wh=1;
            gl_setup_frame(&ww,&wh);
            if (frozen.id)
                draw_tex_rect(frozen,0,0,VW,VH,1,1,1,1);
            else
                draw_solid(0,0,VW,VH,0,0,0,1);
            draw_solid(0,0,VW,VH,0,0,0,0.48f*e);
            const float cw=w*e;
            const float ch=h*e;
            draw_solid(x+(w-cw)*0.5f,y+(h-ch)*0.5f,cw,ch,
                       0.005f,0.008f,0.016f,0.94f*e);
            present();
            SDL_PumpEvents();
            frontend_present_idle(8);
        }
    };

    auto animate_options_in=[&]() {
        const uint64_t t0=SDL_GetTicks();
        const uint64_t duration=145u;
        while (SDL_GetTicks()-t0 < duration) {
            const float t=(float)(SDL_GetTicks()-t0)/(float)duration;
            const float e=1.0f-(1.0f-t)*(1.0f-t)*(1.0f-t);
            int ww=1,wh=1;
            gl_setup_frame(&ww,&wh);
            if (frozen.id) draw_tex_rect(frozen,0,0,VW,VH,1,1,1,1);
            else draw_solid(0,0,VW,VH,0,0,0,1);
            draw_solid(0,0,VW,VH,0,0,0,0.48f);
            draw_main();
            const float x=790.0f+(1.0f-e)*250.0f;
            draw_solid(x,132,1050.0f*e,810,0.005f,0.008f,0.016f,0.95f*e);
            draw_solid(x,132,1050.0f*e,5,1.0f,0.46f,0.03f,e);
            present();
            SDL_PumpEvents();
            frontend_present_idle(8);
        }
    };

    auto animate_confirm_in=[&]() {
        const uint64_t t0=SDL_GetTicks();
        const uint64_t duration=135u;
        while (SDL_GetTicks()-t0 < duration) {
            const float t=(float)(SDL_GetTicks()-t0)/(float)duration;
            const float e=1.0f-(1.0f-t)*(1.0f-t)*(1.0f-t);
            const bool saved_restart=confirm_restart;
            const bool saved_quit=confirm_quit;
            confirm_restart=false;
            confirm_quit=false;
            draw_pause_base();
            confirm_restart=saved_restart;
            confirm_quit=saved_quit;
            const float w=1120.0f*e;
            const float h=470.0f*e;
            draw_solid(960.0f-w*0.5f,545.0f-h*0.5f,w,h,
                       0.005f,0.008f,0.016f,0.985f*e);
            present();
            SDL_PumpEvents();
            frontend_present_idle(8);
        }
    };

    auto run_quit_iris=[&]() {
        mickey_ui_play_sfx(L"enter.wav");
        const float maxr=(float)std::sqrt((double)(VW*VW+VH*VH))*0.55f;
        const uint64_t t0=SDL_GetTicks();
        const uint64_t duration=640u;
        while (SDL_GetTicks()-t0 < duration) {
            const float t=(float)(SDL_GetTicks()-t0)/(float)duration;
            const float e=t*t*(3.0f-2.0f*t);
            draw_pause_base();
            mickey_pause_draw_iris_mask(maxr*(1.0f-e));
            present();
            SDL_PumpEvents();
            frontend_present_idle(8);
        }
        int ww=1,wh=1;
        gl_setup_frame(&ww,&wh);
        draw_solid(0,0,VW,VH,0,0,0,1);
        present();
    };

    if (!reopen_options) {
        mickey_ui_play_sfx(L"window_open.wav");
        animate_box(78,92,655,895);
    }

    SDL_PumpEvents();
    const Uint8 *ks0=SDL_GetKeyboardState(nullptr);
    auto held0=[&](SDL_Scancode sc)->bool { return ks0 && ks0[sc]!=0; };
    bool prev_up=held0(SDL_SCANCODE_UP) || held0(psx_keybinds_get_button(1,PSX_KB_UP));
    bool prev_down=held0(SDL_SCANCODE_DOWN) || held0(psx_keybinds_get_button(1,PSX_KB_DOWN));
    bool prev_left=held0(SDL_SCANCODE_LEFT) || held0(psx_keybinds_get_button(1,PSX_KB_LEFT));
    bool prev_right=held0(SDL_SCANCODE_RIGHT) || held0(psx_keybinds_get_button(1,PSX_KB_RIGHT));
    bool prev_enter=held0(SDL_SCANCODE_RETURN) || held0(SDL_SCANCODE_KP_ENTER) || held0(psx_keybinds_get_button(1,PSX_KB_CROSS));
    bool prev_esc=held0(SDL_SCANCODE_ESCAPE) || held0(psx_keybinds_get_button(1,PSX_KB_CIRCLE));

    uint64_t accept_after=opened_at+300u;
    auto cancel_confirm=[&]() {
        confirm_restart=false;
        confirm_quit=false;
        confirm_selected=1;
        accept_after=SDL_GetTicks()+180u;
        mickey_ui_play_sfx(L"back.wav");
    };
    auto handle_action=[&](MickeyInputAction action) {
        if (pending!=PendingUiAction::None || action==MickeyInputAction::None)
            return;
        if (action==MickeyInputAction::Confirm && SDL_GetTicks()<accept_after)
            return;

        if (confirm_restart || confirm_quit) {
            if (action==MickeyInputAction::Up || action==MickeyInputAction::Down ||
                action==MickeyInputAction::Left || action==MickeyInputAction::Right) {
                confirm_selected=1-confirm_selected;
                mickey_ui_play_sfx(L"pickup.wav");
            } else if (action==MickeyInputAction::Back) {
                cancel_confirm();
            } else if (action==MickeyInputAction::Confirm) {
                if (confirm_selected!=0) {
                    cancel_confirm();
                } else if (confirm_quit) {
                    if (mickey_profiles_pause_can_exit_to_frontend()) {
                        pending=PendingUiAction::ReturnToFrontend;
                    } else {
                        error_text=tr("cannot_menu");
                        error_until=SDL_GetTicks()+1800u;
                        cancel_confirm();
                    }
                } else if (mickey_profiles_pause_can_restart()) {
                    mickey_ui_play_sfx(L"enter.wav");
                    pending=PendingUiAction::RestartGame;
                } else {
                    error_until=SDL_GetTicks()+1800u;
                    cancel_confirm();
                }
            }
            return;
        }

        if (options_focus) {
            if (action==MickeyInputAction::Up) {
                option_selected=(option_selected+3)%4;
                mickey_ui_play_sfx(L"pickup.wav");
            } else if (action==MickeyInputAction::Down) {
                option_selected=(option_selected+1)%4;
                mickey_ui_play_sfx(L"pickup.wav");
            } else if (action==MickeyInputAction::Left || action==MickeyInputAction::Right) {
                apply_pause_option(action==MickeyInputAction::Left ? -1 : 1);
                mickey_ui_play_sfx(L"back.wav");
            } else if (action==MickeyInputAction::Back) {
                options_focus=false;
                selected=2;
                mickey_ui_play_sfx(L"back.wav");
            }
            return;
        }

        if (action==MickeyInputAction::Up || action==MickeyInputAction::Down) {
            move_main(action==MickeyInputAction::Up ? -1 : 1);
        } else if (action==MickeyInputAction::Back) {
            mickey_ui_play_sfx(L"back.wav");
            pending=PendingUiAction::Resume;
        } else if (action==MickeyInputAction::Confirm) {
            mickey_ui_play_sfx(L"enter.wav");
            if (selected==0) {
                pending=PendingUiAction::Resume;
            } else if (selected==1) {
                if (mickey_profiles_pause_can_restart()) {
                    confirm_restart=true;
                    confirm_quit=false;
                    confirm_selected=1;
                    animate_confirm_in();
                    accept_after=SDL_GetTicks()+180u;
                }
            } else if (selected==2) {
                animate_options_in();
                options_focus=true;
                option_selected=0;
                accept_after=SDL_GetTicks()+180u;
            } else if (selected==3) {
                confirm_restart=false;
                confirm_quit=true;
                confirm_selected=1;
                animate_confirm_in();
                accept_after=SDL_GetTicks()+180u;
            }
        }
    };

    for (; pending==PendingUiAction::None;) {
        mickey_ra_idle();
        int ww=1,wh=1;
        gl_setup_frame(&ww,&wh);
        if (frozen.id)
            draw_tex_rect(frozen,0,0,VW,VH,1,1,1,1);
        else
            draw_solid(0,0,VW,VH,0,0,0,1);

        draw_solid(0,0,VW,VH,0,0,0,0.48f);
        draw_main();
        draw_options();
        if (confirm_restart || confirm_quit)
            draw_confirm();
        if (SDL_GetTicks()<error_until) {
            draw_solid(690,958,540,72,0.16f,0.025f,0.018f,0.96f);
            draw_front_text(error_text,735,979,22,1.0f,0.76f,0.62f,1.0f);
        }
        present();

        SDL_PumpEvents();
        const Uint8 *ks=SDL_GetKeyboardState(nullptr);
        auto held=[&](SDL_Scancode sc)->bool { return ks && ks[sc]!=0; };
        const bool cur_up=held(SDL_SCANCODE_UP) || held(psx_keybinds_get_button(1,PSX_KB_UP));
        const bool cur_down=held(SDL_SCANCODE_DOWN) || held(psx_keybinds_get_button(1,PSX_KB_DOWN));
        const bool cur_left=held(SDL_SCANCODE_LEFT) || held(psx_keybinds_get_button(1,PSX_KB_LEFT));
        const bool cur_right=held(SDL_SCANCODE_RIGHT) || held(psx_keybinds_get_button(1,PSX_KB_RIGHT));
        const bool cur_enter=held(SDL_SCANCODE_RETURN) || held(SDL_SCANCODE_KP_ENTER) || held(psx_keybinds_get_button(1,PSX_KB_CROSS));
        const bool cur_esc=held(SDL_SCANCODE_ESCAPE) || held(psx_keybinds_get_button(1,PSX_KB_CIRCLE));
        if (cur_up && !prev_up) { mickey_input_mark_keyboard(); handle_action(MickeyInputAction::Up); }
        if (cur_down && !prev_down) { mickey_input_mark_keyboard(); handle_action(MickeyInputAction::Down); }
        if (cur_left && !prev_left) { mickey_input_mark_keyboard(); handle_action(MickeyInputAction::Left); }
        if (cur_right && !prev_right) { mickey_input_mark_keyboard(); handle_action(MickeyInputAction::Right); }
        if (cur_enter && !prev_enter) { mickey_input_mark_keyboard(); handle_action(MickeyInputAction::Confirm); }
        if (cur_esc && !prev_esc) { mickey_input_mark_keyboard(); handle_action(MickeyInputAction::Back); }
        prev_up=cur_up; prev_down=cur_down;
        prev_left=cur_left; prev_right=cur_right;
        prev_enter=cur_enter; prev_esc=cur_esc;

        SDL_Event e;
        while (pending==PendingUiAction::None && SDL_PollEvent(&e)) {
            bool mouse_click=false;
            float mx=0,my=0;
#if defined(PSX_SDL3)
            if (e.type==SDL_EVENT_QUIT)
                pending=PendingUiAction::Quit;
            if (e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const std::string source=mickey_gamepad_button_source((int)e.gbutton.button);
                mickey_input_mark_gamepad();
                if (mickey_pad_binding_contains(14,source) &&
                    !options_focus && !confirm_restart && !confirm_quit &&
                    SDL_GetTicks()-opened_at>300u) {
                    mickey_ui_play_sfx(L"back.wav");
                    pending=PendingUiAction::Resume;
                } else {
                    handle_action(mickey_action_from_pad_source(source));
                }
            }
            if (e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button==SDL_BUTTON_LEFT) {
                mouse_click=true;
                mouse_virtual(e.button.x,e.button.y,mx,my);
            }
#else
            if (e.type==SDL_QUIT)
                pending=PendingUiAction::Quit;
            if (e.type==SDL_CONTROLLERBUTTONDOWN) {
                const std::string source=mickey_gamepad_button_source((int)e.cbutton.button);
                mickey_input_mark_gamepad();
                if (mickey_pad_binding_contains(14,source) &&
                    !options_focus && !confirm_restart && !confirm_quit &&
                    SDL_GetTicks()-opened_at>300u) {
                    mickey_ui_play_sfx(L"back.wav");
                    pending=PendingUiAction::Resume;
                } else {
                    handle_action(mickey_action_from_pad_source(source));
                }
            }
            if (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
                mouse_click=true;
                mouse_virtual((float)e.button.x,(float)e.button.y,mx,my);
            }
#endif
            if (!mouse_click || pending!=PendingUiAction::None ||
                SDL_GetTicks()<accept_after)
                continue;
            if (confirm_restart || confirm_quit) {
                if (my>=585 && my<=675) {
                    if (mx>=645 && mx<=890) confirm_selected=0;
                    else if (mx>=1025 && mx<=1270) confirm_selected=1;
                    else continue;
                    handle_action(MickeyInputAction::Confirm);
                }
            } else if (!options_focus && mx>=126 && mx<=681) {
                const float ys[4]={330.0f,450.0f,570.0f,690.0f};
                for (int i=0;i<4;i++) {
                    if (my>=ys[i] && my<=ys[i]+82.0f) {
                        selected=i;
                        handle_action(MickeyInputAction::Confirm);
                        break;
                    }
                }
            }
        }
        if (pending==PendingUiAction::None) frontend_present_idle(8);
    }
    if (pending==PendingUiAction::ReturnToFrontend)
        run_quit_iris();
    return finish(pending);
}

extern "C" uint16_t mickey_frontend_pause_filter_buttons(uint16_t buttons)
{
    /* MICKEY_NEWGAME_TITLE_INPUT_GUARD_V1 */
    if (g_mickey_newgame_title_input_guard) {
        if (buttons==0xFFFFu) {
            ++g_mickey_newgame_title_neutral_frames;
            if (g_mickey_newgame_title_neutral_frames>=4) {
                g_mickey_newgame_title_input_guard=false;
                g_mickey_newgame_title_neutral_frames=0;
                std::fprintf(stdout,
                    "MICKEY_NEWGAME_TITLE_INPUT_GUARD_V1: released; next press belongs to original title\n");
                std::fflush(stdout);
            }
        } else {
            g_mickey_newgame_title_neutral_frames=0;
        }

        /* Keep this frame neutral too. The next intentional press is a clean
         * new edge and can advance the original title normally. */
        return 0xFFFFu;
    }

    const bool start_pressed=(buttons & 0x0008u)==0u;

    if (g_initialized && g_window &&
        start_pressed && !g_mickey_pause_start_held &&
        mickey_profiles_pause_available()) {
        g_mickey_pause_requested=true;
    }

    g_mickey_pause_start_held=start_pressed;

    /* START is host-owned during gameplay: never allow the same physical press
     * to open Mickey's original PS1 pause underneath our custom overlay. Keep
     * swallowing it until the player actually releases the button. */
    if (g_initialized && g_window &&
        mickey_profiles_pause_available() &&
        (start_pressed || g_mickey_pause_requested))
        buttons=(uint16_t)(buttons | 0x0008u);

    /* V4 bridge frame: nie przepuszczaj Left/Right uzytego do zmiany
     * ustawienia hosta do Mickeya podczas automatycznego reopen Pause. */
    if (g_pending_ui_action!=PendingUiAction::None || g_mickey_pause_reopen_options)
        buttons=0xFFFFu;

    return buttons;
}

static void mickey_pause_present_tick()
{
    if (!g_initialized || !g_window)
        return;

    SDL_PumpEvents();
    const Uint8 *ks=SDL_GetKeyboardState(nullptr);
    const bool esc=ks && ks[SDL_SCANCODE_ESCAPE]!=0;

    if (esc && !g_mickey_pause_escape_held &&
        mickey_profiles_pause_available())
        g_mickey_pause_requested=true;

    g_mickey_pause_escape_held=esc;

    if (!g_mickey_pause_requested || g_ui_modal_owner!=UiModalOwner::None ||
        g_pending_ui_action!=PendingUiAction::None)
        return;

    /* Do not stop halfway through the opening iris or while the autosave toast
     * corresponds to a still-pending savestate operation. The request remains
     * latched and opens on the first safe presentation frame. */
    if (g_game_open_active || mickey_profiles_saving_active())
        return;

    if (!mickey_profiles_pause_available()) {
        g_mickey_pause_requested=false;
        return;
    }

    g_mickey_pause_requested=false;
    const PendingUiAction action=mickey_pause_dialog();
    if (action!=PendingUiAction::None) {
        g_pending_ui_action=action;
        if (action==PendingUiAction::ReturnToFrontend)
            g_mickey_frontend_return_black_hold=true;
    }
}

extern "C" void mickey_frontend_draw_game_transition(void) {
    /* MICKEY_RA_I18N_V1: evaluate official RA conditions once per presented guest frame. */
    mickey_ra_do_frame();
    ra_poll_unlock_notifications();
    /* Pause polling must stay on the normal present path. */
    mickey_pause_present_tick();
    if (!g_initialized || !g_window) return;

    auto draw_black_hold=[&]() {
        int ww=1,wh=1;
        gl_setup_frame(&ww,&wh);
        draw_solid(0,0,VW,VH,0,0,0,1);
        pBindVertexArray(0);
        pUseProgram(0);
        glBindTexture(GL_TEXTURE_2D,0);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
    };

    /* MICKEY_LOADING_TIPS_SAVE_I18N_V1_HELPERS
     *
     * Loading overlay for the black handoff:
     * - localized "Loading..." label with bouncing letters,
     * - one short localized gameplay tip shown below,
     * - save icon label translated to all supported languages.
     */
    static const char* g_loading_tips_pl[] = {
        u8"Oszczędzaj kulki — przydadzą się na bossów!",
        u8"Skacz na wrogów, gdy możesz.",
        u8"Gwiazdki odnawiają zdrowie Mickey’ego.",
        u8"Czapka Mickey’ego daje dodatkowe życie.",
        u8"Kucaj — niektórych ataków można tak uniknąć.",
        u8"Rozglądaj się — poziomy skrywają sekrety.",
        u8"Nie pędź! Pułapki często pojawiają się znienacka.",
        u8"Po trafieniu jesteś chwilowo nietykalny.",
        u8"Obserwuj bossów — ich ataki się powtarzają.",
        u8"Utknąłeś? Szukaj przełączników i ruchomych obiektów."
    };

    static const char* g_loading_tips_en[] = {
        u8"Save your marbles — they come in handy against bosses!",
        u8"Jump on enemies when you can.",
        u8"Stars restore Mickey's health.",
        u8"Mickey's hat grants an extra life.",
        u8"Crouch — you can dodge some attacks that way.",
        u8"Look around — the stages hide secrets.",
        u8"Don't rush! Traps often appear out of nowhere.",
        u8"After taking a hit, you're invincible for a short while.",
        u8"Watch the bosses — their attack patterns repeat.",
        u8"Stuck? Look for switches and moving objects."
    };

    static const char* g_loading_tips_it[] = {
        u8"Conserva le biglie: ti serviranno contro i boss!",
        u8"Salta sui nemici quando puoi.",
        u8"Le stelle ripristinano la salute di Topolino.",
        u8"Il cappello di Topolino ti dà una vita extra.",
        u8"Abbassati: così puoi evitare alcuni attacchi.",
        u8"Guardati intorno: i livelli nascondono segreti.",
        u8"Non correre! Le trappole spuntano spesso all'improvviso.",
        u8"Dopo un colpo, sei invincibile per un attimo.",
        u8"Osserva i boss: i loro attacchi si ripetono.",
        u8"Bloccato? Cerca interruttori e oggetti mobili."
    };

    static const char* g_loading_tips_ja[] = {
        u8"ビー玉は節約しよう。ボス戦で役立つよ！",
        u8"できるときは敵を踏んで倒そう。",
        u8"スターでミッキーの体力が回復するよ。",
        u8"ミッキーの帽子で残機が1つ増えるよ。",
        u8"しゃがめば避けられる攻撃もあるよ。",
        u8"よく見てみよう。ステージには秘密があるよ。",
        u8"急ぎすぎないで！ 罠は突然現れることがあるよ。",
        u8"ダメージ後は少しの間だけ無敵だよ。",
        u8"ボスを観察しよう。攻撃パターンは繰り返すよ。",
        u8"行き詰まった？ スイッチや動く仕掛けを探してみよう。"
    };

    static const char* g_loading_tips_de[] = {
        u8"Spare deine Murmeln — gegen Bosse sind sie Gold wert!",
        u8"Springe auf Gegner, wenn du kannst.",
        u8"Sterne stellen Mickys Gesundheit wieder her.",
        u8"Mickys Mütze schenkt dir ein Extraleben.",
        u8"Duck dich — so kannst du manchen Angriffen ausweichen.",
        u8"Schau dich um — die Level verbergen Geheimnisse.",
        u8"Überstürze nichts! Fallen tauchen oft plötzlich auf.",
        u8"Nach einem Treffer bist du kurzzeitig unverwundbar.",
        u8"Beobachte Bosse — ihre Angriffsmuster wiederholen sich.",
        u8"Festgefahren? Suche nach Schaltern und beweglichen Objekten."
    };

    auto mickey_tip_lang_index=[&]() -> int {
        return std::clamp(g_cfg.language,0,4);
    };

    auto mickey_loading_label=[&](int lang) -> const char* {
        switch (lang) {
            case 0: return u8"Ładowanie...";
            case 2: return u8"Caricamento...";
            case 3: return u8"ロード中...";
            case 4: return u8"Wird geladen...";
            default: return u8"Loading...";
        }
    };

    auto mickey_saving_label_localized=[&](int lang) -> const char* {
        switch (lang) {
            case 0: return u8"Zapisywanie...";
            case 2: return u8"Salvataggio...";
            case 3: return u8"セーブ中...";
            case 4: return u8"Speichern...";
            default: return u8"Saving...";
        }
    };

    auto mickey_tip_array_for_lang=[&](int lang) -> const char** {
        switch (lang) {
            case 0: return g_loading_tips_pl;
            case 2: return g_loading_tips_it;
            case 3: return g_loading_tips_ja;
            case 4: return g_loading_tips_de;
            default: return g_loading_tips_en;
        }
    };

    /* MICKEY_LOADING_TIPS_SAVE_I18N_HOTFIX_V1_1
     * Use the frontend real glyph renderer instead of guessed functions.
     */
    auto mickey_measure_front_text=[&](const char* s,float height) -> float {
        if (!s) return 0.0f;
        float width=0.0f;
        const unsigned char* p=(const unsigned char*)s;
        while (*p) {
            uint32_t c=mickey_utf8_next(p);
            if (c==' ') {
                width+=height*0.42f;
                continue;
            }
            const Tex* t=nullptr;
            if (c<128 && glyph_tex[c].id) t=&glyph_tex[c];
            else {
                auto it=glyph_unicode.find(c);
                if (it!=glyph_unicode.end() && it->second.id) t=&it->second;
            }
            if (!t || t->h<=0) {
                width+=height*0.46f;
                continue;
            }
            const float w=height*(float)t->w/(float)t->h;
            width+=w*0.86f;
        }
        return width;
    };

    auto mickey_draw_centered_front_text=[&](const char* s,float y,float height,
                                               float r,float g,float b,float a) {
        const float w=mickey_measure_front_text(s,height);
        draw_front_text(s,(VW-w)*0.5f,y,height,r,g,b,a);
    };

    auto mickey_draw_bouncy_loading=[&](const char* s,float y,float height,
                                         float r,float g,float b,float a) {
        if (!s) return;
        const uint32_t ticks=SDL_GetTicks();
        const float total=mickey_measure_front_text(s,height);
        float pen=(VW-total)*0.5f;
        const unsigned char* p=(const unsigned char*)s;
        int glyph_index=0;

        while (*p) {
            uint32_t c=mickey_utf8_next(p);
            if (c==' ') {
                pen+=height*0.42f;
                ++glyph_index;
                continue;
            }

            const Tex* t=nullptr;
            if (c<128 && glyph_tex[c].id) t=&glyph_tex[c];
            else {
                auto it=glyph_unicode.find(c);
                if (it!=glyph_unicode.end() && it->second.id) t=&it->second;
            }

            if (!t || t->h<=0) {
                pen+=height*0.46f;
                ++glyph_index;
                continue;
            }

            const float w=height*(float)t->w/(float)t->h;
            const int phase=(int)(((ticks/85u)+(uint32_t)glyph_index) % 8u);
            static const float jump[8]={0.0f,-2.0f,-5.0f,-8.0f,-5.0f,-2.0f,0.0f,0.0f};
            draw_tex_rect(*t,pen,y+jump[phase],w,height,r,g,b,a);
            pen+=w*0.86f;
            ++glyph_index;
        }
    };

    auto draw_loading_overlay=[&]() {
        draw_black_hold();

        /* MICKEY_LOADING_AFTER_NEWGAME_HOTFIX_V1_2_GL_REBIND
         * draw_black_hold() intentionally tears down frontend GL state.
         * Re-bind it before drawing Loading/tips, otherwise the glyph draw
         * calls execute but nothing is visible. */
        int loading_ww=1,loading_wh=1;
        gl_setup_frame(&loading_ww,&loading_wh);

        const int lang = mickey_tip_lang_index();
        const char* loading = mickey_loading_label(lang);
        const char** tips = mickey_tip_array_for_lang(lang);

        const uint32_t ticks = SDL_GetTicks();
        const int tip_index = (int)((ticks / 2500u) % 10u);
        int y_loading = 300;
        int y_tip = 356;

        /* These text helpers are expected to already exist in the custom frontend.
         * If your local source uses different helper names, only these calls
         * need adapting by hand.
         */
        mickey_draw_bouncy_loading(loading,(float)y_loading,38.0f,1.0f,0.72f,0.16f,1.0f);
        mickey_draw_centered_front_text(tips[tip_index],(float)y_tip,23.0f,0.84f,0.84f,0.82f,1.0f);
            /* MICKEY_LOADING_AFTER_NEWGAME_HOTFIX_V1_2 */
        pBindVertexArray(0);
        pUseProgram(0);
        glBindTexture(GL_TEXTURE_2D,0);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
    };


    /* MICKEY_HIDE_ONLY_ANTIPIRACY_V2_2_3
     *
     * Ukrywamy TYLKO pierwsza plansze anti-piracy.
     * Sony, Disney Interactive Presents i original title zostaja widoczne.
     *
     * Maska:
     * - wlacza sie natychmiast po wyjsciu z custom frontendu,
     * - czeka az anti-piracy faktycznie pojawi sie pod spodem,
     * - ignoruje fade/animacje tej planszy,
     * - schodzi dopiero podczas prawdziwej czarnej przerwy PO niej.
     *
     * Nie zmieniamy guest flow.
     */
    {
        static bool ap223_session=false;
        static bool ap223_saw_card=false;
        static bool ap223_reveal=false;
        static uint64_t ap223_start_ms=0;
        static int ap223_card_frames=0;
        static int ap223_black_frames=0;

        const bool ap223_active=mickey_profiles_title_boot_mask_active()!=0;

        if (!ap223_active) {
            ap223_session=false;
            ap223_saw_card=false;
            ap223_reveal=false;
            ap223_start_ms=0;
            ap223_card_frames=0;
            ap223_black_frames=0;
        } else {
            if (!ap223_session) {
                ap223_session=true;
                ap223_saw_card=false;
                ap223_reveal=false;
                ap223_start_ms=SDL_GetTicks();
                ap223_card_frames=0;
                ap223_black_frames=0;

                std::fprintf(stdout,
                    "MICKEY_HIDE_ONLY_ANTIPIRACY_V2_2_3: mask armed immediately\n");
                std::fflush(stdout);
            }

            if (!ap223_reveal) {
                int fw=0,fh=0;
                SDL_GL_GetDrawableSize(g_window,&fw,&fh);

                if (fw>=160 && fh>=120) {
                    unsigned char px[160*120*3];

                    const int sx=std::max(0,fw/2-80);
                    const int sy=std::max(0,fh/2-60);

                    glReadPixels(
                        sx,sy,160,120,
                        GL_RGB,GL_UNSIGNED_BYTE,
                        px
                    );

                    const int pixels=160*120;
                    unsigned long long lum_sum=0;
                    int bright_pixels=0;

                    for (int j=0;j<pixels;j++) {
                        const int r=px[j*3+0];
                        const int g=px[j*3+1];
                        const int b=px[j*3+2];

                        const int lum=(r*54 + g*183 + b*19) >> 8;
                        lum_sum+=(unsigned long long)lum;

                        if (lum>=96)
                            ++bright_pixels;
                    }

                    const double avg_lum=(double)lum_sum/(double)pixels;
                    const double bright_ratio=(double)bright_pixels/(double)pixels;
                    const uint64_t elapsed=SDL_GetTicks()-ap223_start_ms;

                    if (!ap223_saw_card) {
                        const bool visible_card=
                            elapsed>=80u &&
                            (
                                avg_lum>=3.5 ||
                                bright_ratio>=0.0015
                            );

                        if (visible_card) {
                            ++ap223_card_frames;

                            if (ap223_card_frames>=2) {
                                ap223_saw_card=true;
                                ap223_black_frames=0;

                                std::fprintf(stdout,
                                    "MICKEY_HIDE_ONLY_ANTIPIRACY_V2_2_3: anti-piracy locked under mask\n");
                                std::fflush(stdout);
                            }
                        } else {
                            ap223_card_frames=0;
                        }
                    } else {
                        /* Reveal ONLY on a genuine black inter-card gap.
                         * Fade motion itself is ignored. */
                        const bool true_black_gap=
                            avg_lum<=1.8 &&
                            bright_ratio<=0.00030;

                        if (true_black_gap) {
                            ++ap223_black_frames;
                        } else {
                            ap223_black_frames=0;
                        }

                        if (ap223_black_frames>=3) {
                            ap223_reveal=true;

                            std::fprintf(stdout,
                                "MICKEY_HIDE_ONLY_ANTIPIRACY_V2_2_3: black gap -> reveal Sony/Disney\n");
                            std::fflush(stdout);
                        }
                    }

                    /* Driver failsafe; intentionally late. */
                    if (!ap223_reveal && elapsed>=11000u) {
                        ap223_reveal=true;

                        std::fprintf(stdout,
                            "MICKEY_HIDE_ONLY_ANTIPIRACY_V2_2_3: 11s failsafe -> reveal\n");
                        std::fflush(stdout);
                    }
                }

                if (!ap223_reveal) {
                    draw_loading_overlay(); /* MICKEY_LOADING_TIPS_SAVE_I18N_V1_HOOK */
                    return;
                }
            }
        }
    }

    /* Modal exits only draw the hold frame here. Their state-changing actions
     * run at the host vblank boundary after this present fully returns. */
    if (g_gameover_deferred_load_black_hold) {
        draw_black_hold();
        return;
    }

    /* Pause -> Exit Game: the guest first restores the hidden ORIGINAL main-menu
     * savestate. Until the load callback reports success, keep this exact same
     * SDL window black. No CreateProcess, no exit(), no close/reopen flash. */
    if (g_mickey_frontend_return_black_hold) {
        draw_black_hold();
        return;
    }

    /* CONTINUE from a re-entered PC menu requests a savestate load immediately.
     * Do not reveal hidden slot 8 for a frame while the profile load is pending. */
    if (g_game_open_active && mickey_profiles_frontend_load_pending()) {
        draw_black_hold();
        return;
    }

    const bool saving=mickey_profiles_saving_active()!=0;
    const bool show_toast=g_ra_toast_current.active;
    if (!g_game_open_active && !saving && !show_toast) return;

    uint64_t now=SDL_GetTicks();
    bool frame_ready=false;

    if (g_game_open_active) {
        if (!g_game_open_start) g_game_open_start=now;
        const float duration=0.62f;
        float t=(float)(now-g_game_open_start)/(duration*1000.0f);
        if (t>=1.0f) {
            g_game_open_active=false;
        } else {
            int ww=1,wh=1;
            gl_setup_frame(&ww,&wh);
            frame_ready=true;
            float r=max_circle_radius()*ease(t);
            draw_circle_mask(r);
        }
    }

    if (saving) {
        if (!frame_ready) {
            int ww=1,wh=1;
            gl_setup_frame(&ww,&wh);
            frame_ready=true;
        }
        const float x=705.0f,y=902.0f,w=510.0f,h=112.0f;
        draw_solid(x,y,w,h,0.005f,0.008f,0.016f,0.82f);
        draw_solid(x,y,w,3,1.0f,0.47f,0.04f,0.95f);
        draw_front_text(
            mickey_saving_label_localized(std::clamp(g_cfg.language,0,4)),
            x+42,y+35,27.0f,0.96f,0.96f,0.94f,1.0f);

        const int hot=(int)((now/90u)%8u);
        const float cx=x+w-58.0f, cy=y+h*0.5f;
        for(int i=0;i<8;i++) {
            const float a=(float)i*6.28318530718f/8.0f;
            const float sx=cx+std::cos(a)*25.0f-5.0f;
            const float sy=cy+std::sin(a)*25.0f-5.0f;
            if (i==hot) draw_solid(sx,sy,10,10,1.0f,0.55f,0.08f,1.0f);
            else draw_solid(sx,sy,8,8,0.86f,0.86f,0.82f,0.46f);
        }
    }

    if (show_toast) {
        if (!frame_ready) {
            int ww=1,wh=1;
            gl_setup_frame(&ww,&wh);
            frame_ready=true;
        }
        ra_draw_unlock_toast();
    }

    if (frame_ready) {
        pBindVertexArray(0);
        pUseProgram(0);
        glBindTexture(GL_TEXTURE_2D,0);
        glDisable(GL_BLEND);
    }
}

/* One owner for modal commits. This runs from the host vblank wrapper, after
 * sdl_vblank_present_body() has returned and all renderer frames are finished. */
extern "C" void mickey_frontend_host_frame_tick(void)
{
    if (g_ui_modal_owner!=UiModalOwner::None)
        return;
    const PendingUiAction action=g_pending_ui_action;
    g_pending_ui_action=PendingUiAction::None;
    if (action==PendingUiAction::ApplyDisplaySettings) {
        const bool window_change=g_pending_display_window_change;
        g_pending_display_window_change=false;
        if (g_initialized && g_window) {
            if (window_change)
                apply_window_settings();
            else
                apply_game_settings();
            gl_renderer_invalidate_present();
            g_mickey_pause_requested=true;
        } else {
            g_mickey_pause_reopen_options=false;
        }
    } else if (action==PendingUiAction::RestartGame) {
        if (!mickey_profiles_pause_restart())
            std::fprintf(stderr,"mickey_frontend: deferred Pause restart refused\n");
    } else if (action==PendingUiAction::ReturnToFrontend) {
        if (!mickey_profiles_pause_exit_to_frontend()) {
            g_mickey_frontend_return_black_hold=false;
            std::fprintf(stderr,"mickey_frontend: deferred Pause return refused\n");
        }
    } else if (action==PendingUiAction::Quit) {
        std::exit(0);
    }

    if (g_gameover_deferred_load_black_hold) {
        const int submit=mickey_profiles_gameover_deferred_load_tick();
        if (submit<0 || !mickey_profiles_gameover_deferred_load_waiting())
            g_gameover_deferred_load_black_hold=false;
    }

    if (g_mickey_frontend_return_black_hold) {
        const int submit=mickey_profiles_pause_frontend_deferred_load_tick();
        if (submit<0) {
            g_mickey_frontend_return_black_hold=false;
            return;
        }
        const int result=mickey_profiles_take_frontend_return_result();
        if (result<0) {
            g_mickey_frontend_return_black_hold=false;
        } else if (result>0) {
            g_mickey_frontend_return_black_hold=false;
            g_game_open_active=false;
            g_game_open_start=0;
            std::fprintf(stdout,"MICKEY_PAUSE_EXIT_V45_PRESENT_SAFELOAD: slot8 OK -> frontend reenter\n");
            std::fflush(stdout);
            mickey_frontend_run(g_window);
        }
    }
}


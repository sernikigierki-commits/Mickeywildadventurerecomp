#include "mickey_profiles.h"
#include "mickey_ra.h"
#include "savestate.h"
#include "mickey_frontend.h"
#include "starvation_ring.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

extern "C" uint8_t *g_psx_ram;

namespace fs = std::filesystem;

namespace {

constexpr int kProfileCount = 3;
/* Keep the three PC story profiles in the LAST three psxrecomp slots.
 * Normal F7 users almost always stay in the low-numbered slots, so profile
 * checkpoints are much less likely to be overwritten accidentally. */
constexpr int kStateSlotBase = 9;
constexpr int kMenuStateSlot = 8;  /* hidden checkpoint of the original in-game main menu */

/* SCES_001.63 (Europe), gp = 0x800AFBC0.
 * 0x800B02B4 = gp+1780: try/life counter (decremented on lost try).
 * 0x800AFFC4 = gp+1028: marble counter (starts at 10, pickup paths cap at 99,
 *                       throw paths decrement it).
 */
constexpr uint32_t kLivesAddr   = 0x800B02B4u;
constexpr uint32_t kMarblesAddr = 0x800AFFC4u;

const char *kWorldNames[] = {
    "STEAMBOAT WILLIE",
    "THE MAD DOCTOR",
    "MOOSE HUNTERS",
    "THE LONESOME GHOSTS",
    "MICKEY AND THE BEANSTALK",
    "THE PRINCE AND THE PAUPER",
    "THE BAND CONCERT"
};
constexpr int kWorldCount = (int)(sizeof(kWorldNames) / sizeof(kWorldNames[0]));

struct ProfileRecord {
    bool exists = false;
    int world_id = -1;
    int lives = -1;
    int marbles = -1;
    int initial_lives = -1;
    int64_t timestamp = 0;
    std::string state_path;
    std::string date_text;
};

fs::path g_base_dir;
ProfileRecord g_profiles[kProfileCount];
int g_active_profile = -1;
int g_last_story_world = -1;
int g_current_world = -1;
bool g_practice = false;
bool g_configured = false;
bool g_continue_pending = false;
bool g_new_game_pending = false;
/* MICKEY_NEWGAME_TO_ORIGINAL_TITLE_V2 */
bool g_new_game_title_boot_mask = false;
bool g_delete_pending[kProfileCount] = {false,false,false};
bool g_save_pending = false;
int g_save_world = -1;
int g_initial_lives = -1;
bool g_menu_snapshot_pending = false;
bool g_gameover_restart_pending = false;
bool g_gameover_restart_deferred = false; /* MICKEY_GAMEOVER_V43_PRESENT_SAFELOAD */
bool g_gameover_menu_deferred = false;
bool g_gameover_menu_native_fallback = false; /* MICKEY_GAMEOVER_V44_MENU_POSTCLEANUP */
bool g_gameover_resave_pending = false;
bool g_gameover_pending = false;
bool g_return_to_menu_pending = false;
bool g_return_to_frontend_pending = false;
bool g_pause_frontend_load_deferred = false; /* MICKEY_PAUSE_EXIT_V45_PRESENT_SAFELOAD */
int g_frontend_return_result = 0;
std::chrono::steady_clock::time_point g_save_visible_until{};

/* MICKEY_GAMEOVER_V23_GL_SAFELOAD
 * Persistent diagnostics because release is linked with -mwindows. */
void gameover_debug(const char *fmt, ...) {
    fs::path root = g_base_dir.empty() ? fs::current_path() : g_base_dir;
    fs::path path = root / "gameover_v23_debug.txt";
    std::ofstream f(path, std::ios::out | std::ios::app);
    if (!f) return;
    char msg[1200]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char ts[32]{};
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d",
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    f << "[" << ts << "] " << msg << "\n";
    f.flush();
}

int state_slot(int profile) {
    return kStateSlotBase + profile;
}

bool valid_profile(int p) {
    return p >= 0 && p < kProfileCount;
}

bool valid_world(int w) {
    return w >= 0 && w < kWorldCount;
}

uint16_t read_guest_u16(uint32_t addr) {
    if (!g_psx_ram) return 0;
    const uint32_t phys = addr & 0x1FFFFFu;
    if (phys + 1u >= 0x200000u) return 0;
    return (uint16_t)((uint16_t)g_psx_ram[phys] |
                      ((uint16_t)g_psx_ram[phys + 1u] << 8));
}

void write_guest_u16(uint32_t addr, uint16_t value) {
    if (!g_psx_ram) return;
    const uint32_t phys = addr & 0x1FFFFFu;
    if (phys + 1u >= 0x200000u) return;
    g_psx_ram[phys] = (uint8_t)(value & 0xFFu);
    g_psx_ram[phys + 1u] = (uint8_t)((value >> 8) & 0xFFu);
}

fs::path profile_dir() {
    fs::path root = g_base_dir.empty() ? fs::current_path() : g_base_dir;
    return root / "profiles";
}

fs::path meta_path(int profile) {
    char name[32];
    std::snprintf(name, sizeof(name), "slot%d.meta", profile + 1);
    return profile_dir() / name;
}

fs::path bmp_path(int profile) {
    char name[32];
    std::snprintf(name, sizeof(name), "slot%d.bmp", profile + 1);
    return profile_dir() / name;
}

std::string local_date_text(int64_t ts) {
    std::time_t t = (std::time_t)ts;
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d.%02d.%04d %02d:%02d",
                  tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900,
                  tmv.tm_hour, tmv.tm_min);
    return buf;
}

void load_one(int profile) {
    ProfileRecord rec{};
    std::ifstream f(meta_path(profile));
    if (!f) {
        g_profiles[profile] = rec;
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        try {
            if (k == "world_id") rec.world_id = std::stoi(v);
            else if (k == "lives") rec.lives = std::stoi(v);
            else if (k == "marbles") rec.marbles = std::stoi(v);
            else if (k == "initial_lives") rec.initial_lives = std::stoi(v);
            else if (k == "timestamp") rec.timestamp = std::stoll(v);
            else if (k == "state_path") rec.state_path = v;
            else if (k == "date") rec.date_text = v;
        } catch (...) {
        }
    }

    rec.exists = valid_world(rec.world_id) && rec.timestamp > 0;
    if (rec.exists && !rec.state_path.empty()) {
        std::error_code ec;
        if (!fs::is_regular_file(fs::path(rec.state_path), ec))
            rec.exists = false;
    }
    if (rec.date_text.empty() && rec.timestamp > 0)
        rec.date_text = local_date_text(rec.timestamp);
    /* v1 profiles did not persist the starting-life setting. Use their
     * checkpoint value as a conservative migration fallback. New v2 saves
     * always store the real value captured at the first story start. */
    if (rec.initial_lives <= 0)
        rec.initial_lives = rec.lives > 0 ? rec.lives : 3;
    g_profiles[profile] = rec;
}

void load_all() {
    if (g_base_dir.empty()) return;
    std::error_code ec;
    fs::create_directories(profile_dir(), ec);
    for (int i = 0; i < kProfileCount; ++i)
        load_one(i);
}

void remove_file_quiet(const fs::path &p) {
    if (p.empty()) return;
    std::error_code ec;
    fs::remove(p, ec);
}

void remove_state_files_now(int profile) {
    if (!valid_profile(profile)) return;

    /* First use the persisted path, so Delete works even before the runtime
     * has configured this session's save directory. */
    const std::string persisted = g_profiles[profile].state_path;
    if (!persisted.empty()) {
        fs::path pst(persisted);
        remove_file_quiet(pst);
        fs::path thumb = pst;
        thumb.replace_extension(".thumb");
        remove_file_quiet(thumb);
    }

    /* Once savestate is configured, also remove the canonical current path. */
    if (g_configured) {
        char path[768]{};
        if (savestate_slot_path(state_slot(profile), path, sizeof(path))) {
            fs::path pst(path);
            remove_file_quiet(pst);
            fs::path thumb = pst;
            thumb.replace_extension(".thumb");
            remove_file_quiet(thumb);
        }
    }
}

void clear_profile_files(int profile) {
    if (!valid_profile(profile)) return;
    remove_state_files_now(profile);
    remove_file_quiet(meta_path(profile));
    remove_file_quiet(bmp_path(profile));
    g_profiles[profile] = ProfileRecord{};
}

void write_u16(std::ofstream &f, uint16_t v) {
    const unsigned char b[2] = {
        (unsigned char)(v & 0xFFu),
        (unsigned char)((v >> 8) & 0xFFu)
    };
    f.write((const char *)b, 2);
}

void write_u32(std::ofstream &f, uint32_t v) {
    const unsigned char b[4] = {
        (unsigned char)(v & 0xFFu),
        (unsigned char)((v >> 8) & 0xFFu),
        (unsigned char)((v >> 16) & 0xFFu),
        (unsigned char)((v >> 24) & 0xFFu)
    };
    f.write((const char *)b, 4);
}

void export_thumb_bmp(int profile) {
    uint32_t argb[SAVESTATE_THUMB_W * SAVESTATE_THUMB_H];
    if (!savestate_read_thumb(state_slot(profile), argb,
                              SAVESTATE_THUMB_W, SAVESTATE_THUMB_H))
        return;

    std::error_code ec;
    fs::create_directories(profile_dir(), ec);
    std::ofstream f(bmp_path(profile), std::ios::binary | std::ios::trunc);
    if (!f) return;

    const uint32_t pixel_bytes = SAVESTATE_THUMB_W * SAVESTATE_THUMB_H * 4u;
    const uint32_t file_size = 54u + pixel_bytes;

    f.put('B'); f.put('M');
    write_u32(f, file_size);
    write_u16(f, 0); write_u16(f, 0);
    write_u32(f, 54u);
    write_u32(f, 40u);
    write_u32(f, SAVESTATE_THUMB_W);
    /* negative height = top-down BMP */
    write_u32(f, (uint32_t)(-(int32_t)SAVESTATE_THUMB_H));
    write_u16(f, 1u);
    write_u16(f, 32u);
    write_u32(f, 0u);
    write_u32(f, pixel_bytes);
    write_u32(f, 2835u); write_u32(f, 2835u);
    write_u32(f, 0u); write_u32(f, 0u);

    for (int i = 0; i < SAVESTATE_THUMB_W * SAVESTATE_THUMB_H; ++i) {
        const uint32_t c = argb[i];
        const unsigned char bgra[4] = {
            (unsigned char)(c & 0xFFu),
            (unsigned char)((c >> 8) & 0xFFu),
            (unsigned char)((c >> 16) & 0xFFu),
            0xFFu
        };
        f.write((const char *)bgra, 4);
    }
}

void persist_profile(int profile, int world_id) {
    if (!valid_profile(profile) || !valid_world(world_id)) return;

    ProfileRecord rec{};
    rec.exists = true;
    rec.world_id = world_id;
    rec.lives = (int)(int16_t)read_guest_u16(kLivesAddr);
    rec.marbles = (int)(int16_t)read_guest_u16(kMarblesAddr);
    rec.initial_lives = g_initial_lives > 0 ? g_initial_lives : rec.lives;
    rec.timestamp = (int64_t)std::time(nullptr);
    rec.date_text = local_date_text(rec.timestamp);

    char state[768]{};
    if (savestate_slot_path(state_slot(profile), state, sizeof(state)))
        rec.state_path = state;

    std::error_code ec;
    fs::create_directories(profile_dir(), ec);
    std::ofstream f(meta_path(profile), std::ios::out | std::ios::trunc);
    if (f) {
        f << "version=2\n";
        f << "world_id=" << rec.world_id << "\n";
        f << "lives=" << rec.lives << "\n";
        f << "marbles=" << rec.marbles << "\n";
        f << "initial_lives=" << rec.initial_lives << "\n";
        f << "timestamp=" << rec.timestamp << "\n";
        f << "date=" << rec.date_text << "\n";
        f << "state_path=" << rec.state_path << "\n";
    }

    g_profiles[profile] = rec;
    export_thumb_bmp(profile);

    std::fprintf(stdout,
        "mickey_profiles: PROFILE %d saved: world=%d '%s' lives=%d marbles=%d\n",
        profile + 1, world_id, kWorldNames[world_id], rec.lives, rec.marbles);
    std::fflush(stdout);
}

void migrate_persisted_state_if_needed(int profile) {
    if (!valid_profile(profile) || !g_profiles[profile].exists) return;
    char current[768]{};
    if (!savestate_slot_path(state_slot(profile), current, sizeof(current))) return;

    fs::path dst(current);
    std::error_code ec;
    if (fs::is_regular_file(dst, ec)) return;

    const std::string old = g_profiles[profile].state_path;
    if (old.empty()) return;
    fs::path src(old);
    if (!fs::is_regular_file(src, ec)) return;

    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        fs::path src_thumb = src; src_thumb.replace_extension(".thumb");
        fs::path dst_thumb = dst; dst_thumb.replace_extension(".thumb");
        if (fs::is_regular_file(src_thumb, ec)) {
            ec.clear();
            fs::copy_file(src_thumb, dst_thumb,
                          fs::copy_options::overwrite_existing, ec);
        }
    }
}

void arm_autosave(int world_id) {
    if (!valid_profile(g_active_profile) || g_practice || !g_configured)
        return;

    g_save_world = world_id;
    g_save_pending = true;
    g_save_visible_until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(1200);

    if (!savestate_request_save(state_slot(g_active_profile))) {
        g_save_pending = false;
        std::fprintf(stderr,
            "mickey_profiles: autosave request refused (profile=%d world=%d practice=%d)\n",
            g_active_profile + 1, world_id, g_practice ? 1 : 0);
    } else {
        std::fprintf(stdout,
            "mickey_profiles: autosave staged -> profile %d, world %d (%s)\n",
            g_active_profile + 1, world_id, kWorldNames[world_id]);
        std::fflush(stdout);
    }
}

} // namespace

extern "C" void mickey_profiles_set_base_dir(const char *base_dir) {
    if (base_dir && base_dir[0])
        g_base_dir = fs::path(base_dir);
    else
        g_base_dir = fs::current_path();
    load_all();
}

extern "C" void mickey_profiles_select_new(int profile) {
    /* MICKEY_NEWGAME_TO_ORIGINAL_TITLE_V2
     * Conceal the original PS1 boot cards after leaving the custom frontend.
     * Reveal only when the real title/attract path is reached. */
    g_new_game_title_boot_mask = true;

    if (!valid_profile(profile)) return;
    g_active_profile = profile;
    g_continue_pending = false;
    g_new_game_pending = true;
    g_practice = false;
    g_last_story_world = -1;
    g_current_world = -1;
    g_save_pending = false;
    g_initial_lives = -1;
    g_gameover_restart_pending = false;
    g_gameover_restart_deferred = false;
    g_gameover_menu_deferred = false;
    g_gameover_resave_pending = false;
    g_return_to_menu_pending = false;
    g_gameover_pending = false;

    /* IMPORTANT: do NOT destroy an existing profile yet. The original game
     * still asks START GAME vs PRACTICE GAME after our frontend. If the player
     * chooses PRACTICE, the old story profile must remain completely untouched.
     * We only commit the overwrite when the first STORY world actually enters. */
    std::fprintf(stdout,
        "mickey_profiles: NEW GAME armed -> profile %d (old save preserved until story starts)\n",
        profile + 1);
    std::fflush(stdout);
}

extern "C" int mickey_profiles_select_continue(int profile) {
    g_new_game_title_boot_mask = false;

    if (!valid_profile(profile)) return 0;
    load_one(profile);
    if (!g_profiles[profile].exists) return 0;

    g_active_profile = profile;
    g_continue_pending = true;
    g_new_game_pending = false;
    g_practice = false;
    g_last_story_world = g_profiles[profile].world_id;
    g_current_world = g_profiles[profile].world_id;
    g_initial_lives = g_profiles[profile].initial_lives > 0
                    ? g_profiles[profile].initial_lives
                    : g_profiles[profile].lives;
    g_save_pending = false;
    g_gameover_restart_pending = false;
    g_gameover_restart_deferred = false;
    g_gameover_menu_deferred = false;
    g_gameover_resave_pending = false;
    g_return_to_menu_pending = false;
    g_gameover_pending = false;

    std::fprintf(stdout, "mickey_profiles: CONTINUE -> profile %d, world %d\n",
                 profile + 1, g_last_story_world);
    std::fflush(stdout);

    if (g_configured)
        mickey_profiles_on_savestate_configured();
    return 1;
}

extern "C" void mickey_profiles_delete(int profile) {
    if (!valid_profile(profile)) return;
    clear_profile_files(profile);
    if (!g_configured)
        g_delete_pending[profile] = true;
    else
        g_delete_pending[profile] = false;

    if (g_active_profile == profile) {
        g_active_profile = -1;
        g_continue_pending = false;
        g_new_game_pending = false;
        g_last_story_world = -1;
        g_current_world = -1;
        g_initial_lives = -1;
        g_gameover_restart_pending = false;
        g_gameover_resave_pending = false;
        g_return_to_menu_pending = false;
    }
    std::fprintf(stdout, "mickey_profiles: DELETE profile %d\n", profile + 1);
    std::fflush(stdout);
}

extern "C" int mickey_profiles_get_info(int profile, MickeyProfileInfo *out) {
    if (!valid_profile(profile) || !out) return 0;
    load_one(profile);
    const ProfileRecord &r = g_profiles[profile];

    std::memset(out, 0, sizeof(*out));
    out->exists = r.exists ? 1 : 0;
    out->world_id = r.world_id;
    out->lives = r.lives;
    out->marbles = r.marbles;
    out->timestamp = r.timestamp;
    if (valid_world(r.world_id))
        std::snprintf(out->world_name, sizeof(out->world_name), "%s",
                      kWorldNames[r.world_id]);
    std::snprintf(out->date_text, sizeof(out->date_text), "%s",
                  r.date_text.c_str());
    return 1;
}

extern "C" int mickey_profiles_active_profile(void) {
    return g_active_profile;
}

extern "C" void mickey_profiles_set_practice(int enabled) {
    const bool next = enabled != 0;
    if (g_practice == next) return;
    g_practice = next;
    if (g_practice) {
        /* Practice can never create/update a save or open Story recovery UI. */
        g_save_pending = false;
        g_gameover_pending = false;
        std::fprintf(stdout, "mickey_profiles: PRACTICE ON -> saves HARD BLOCKED\n");
    } else {
        std::fprintf(stdout, "mickey_profiles: PRACTICE OFF\n");
    }
    std::fflush(stdout);
}

extern "C" int mickey_profiles_is_practice(void) {
    return g_practice ? 1 : 0;
}

extern "C" int mickey_profiles_save_allowed(void) {
    return g_practice ? 0 : 1;
}

extern "C" void mickey_profiles_world_enter(int world_id) {
    if (!valid_world(world_id)) return;
    g_current_world = world_id; /* MICKEY_PAUSE_WORLD_TRACK: state only, never saves */

    /* PRACTICE is a hard no-save mode. It must not overwrite metadata, must not
     * delete an existing profile selected on the frontend, and must not advance
     * the story autosave baseline. */
    if (g_practice) {
        std::fprintf(stdout, "mickey_profiles: practice world %d (%s) -- NO SAVE / NO PROFILE CHANGES\n",
                     world_id, kWorldNames[world_id]);
        std::fflush(stdout);
        return;
    }

    if (!valid_profile(g_active_profile)) return;
    if (g_continue_pending) return; /* wait until the selected state restores */

    g_current_world = world_id;

    if (g_new_game_pending) {
        /* This is the first proof that the player chose START GAME rather than
         * PRACTICE GAME. Only now is it safe to overwrite the selected profile.
         * v2 also checkpoints the FIRST cartoon immediately, because Game Over
         * recovery must be able to restart Steamboat Willie too. */
        const int current_lives = (int)(int16_t)read_guest_u16(kLivesAddr);
        g_initial_lives = (current_lives > 0 && current_lives < 100)
                        ? current_lives : 3;

        clear_profile_files(g_active_profile);
        if (!g_configured)
            g_delete_pending[g_active_profile] = true;
        g_new_game_pending = false;
        g_last_story_world = world_id;
        std::fprintf(stdout,
            "mickey_profiles: STORY START confirmed -> profile %d at %s, initial_lives=%d\n",
            g_active_profile + 1, kWorldNames[world_id], g_initial_lives);
        std::fflush(stdout);

        arm_autosave(world_id);
        return;
    }

    if (g_last_story_world < 0) {
        /* Fallback for a profile armed outside the frontend. */
        g_last_story_world = world_id;
        return;
    }

    if (world_id <= g_last_story_world) {
        /* Restart / backtrack: re-arm progression from this point without saving. */
        g_last_story_world = world_id;
        return;
    }

    /* Entering a later cartoon means the previous one has been completed. The
     * state is staged here, at the beginning/title of the new cartoon. */
    g_last_story_world = world_id;
    arm_autosave(world_id);
}

extern "C" void mickey_profiles_main_menu_enter(void) {
    /* This hook is ONLY for the hidden compatibility checkpoint. v2.2 does
     * not show Game Over UI here. In particular it must never alter RA/PC. */
    g_practice = false;
    if (!g_configured || !g_new_game_pending || g_continue_pending ||
        g_menu_snapshot_pending)
        return;

    char why[128]{};
    if (savestate_slot_exists(kMenuStateSlot) &&
        savestate_slot_compatible(kMenuStateSlot, why, sizeof(why)))
        return;

    g_menu_snapshot_pending = true;
    if (!savestate_request_save(kMenuStateSlot)) {
        g_menu_snapshot_pending = false;
        std::fprintf(stderr,
            "mickey_profiles: could not capture original main-menu checkpoint\\n");
    } else {
        std::fprintf(stdout,
            "mickey_profiles: capturing original in-game main menu -> hidden slot %d\\n",
            kMenuStateSlot);
        std::fflush(stdout);
    }
}

extern "C" int mickey_profiles_game_over(void) {
    /* v2.3 detector only. Original PS1 Game Over animation/jingle must finish. */
    if (g_practice || !valid_profile(g_active_profile) || !g_configured)
        return 0;

    if (!g_gameover_pending) {
        g_gameover_pending = true;
        gameover_debug("ARM profile=%d world=%d practice=%d",
                       g_active_profile + 1, g_current_world, g_practice ? 1 : 0);
        std::fprintf(stdout,
            "MICKEY_GAMEOVER_V23_ARMED: original Game Over continues normally\n");
        std::fflush(stdout);
    }
    return 0;
}

extern "C" int mickey_profiles_game_over_recovery_point(void) {
    if (!g_gameover_pending)
        return MICKEY_GAMEOVER_NONE;

    g_gameover_pending = false;

    if (g_practice || !valid_profile(g_active_profile) || !g_configured) {
        gameover_debug("RECOVERY_SKIPPED practice=%d active=%d configured=%d",
                       g_practice ? 1 : 0, g_active_profile, g_configured ? 1 : 0);
        return MICKEY_GAMEOVER_NONE;
    }

    char why[128]{};
    const int profile_slot = state_slot(g_active_profile);
    const bool can_restart =
        savestate_slot_exists(profile_slot) &&
        savestate_slot_compatible(profile_slot, why, sizeof(why));

    char menu_why[128]{};
    const bool can_menu =
        savestate_slot_exists(kMenuStateSlot) &&
        savestate_slot_compatible(kMenuStateSlot, menu_why, sizeof(menu_why));

    const int lives = g_initial_lives > 0 ? g_initial_lives : 3;
    const char *world = valid_world(g_current_world)
                      ? kWorldNames[g_current_world] : "AKTUALNA BAJKA";

    gameover_debug("HYBRID_V4_2_RECOVERY profile=%d world=%d restart=%d(%s) menu=%d(%s)",
                   g_active_profile + 1, g_current_world,
                   can_restart ? 1 : 0, why[0] ? why : "ok",
                   can_menu ? 1 : 0, menu_why[0] ? menu_why : "ok");

    const int action = mickey_frontend_game_over_dialog(
        world, lives, can_restart ? 1 : 0, can_menu ? 1 : 0);
    /* The guest is intentionally stopped while the host modal waits for input. */
    starvation_watchdog_heartbeat();

    gameover_debug("DIALOG_RETURN action=%d", action);

    if (action == MICKEY_GAMEOVER_RESTART && can_restart) {
        /* MICKEY_GAMEOVER_V43_PRESENT_SAFELOAD
         * Crash dump: slot 9 loads successfully through normal CONTINUE, but the
         * Game Over request dies after accepted=1 and BEFORE PROFILE_LOAD_RESULT.
         * Therefore A598 must only queue the action. The real LOAD is submitted
         * from the normal present path, same context as stable Pause -> Restart. */
        g_gameover_restart_pending = true;
        g_gameover_restart_deferred = true;
        g_gameover_menu_deferred = false;
        g_continue_pending = false;
        gameover_debug("RESTART_DEFERRED_AT_A598 slot=%d", profile_slot);
        return MICKEY_GAMEOVER_RESTART;
    }

    if (action == MICKEY_GAMEOVER_MAIN_MENU && can_menu) {
        /* MICKEY_GAMEOVER_V44_MENU_POSTCLEANUP
         * The v4.3 crash dump proves MENU never reaches the next normal present:
         * log stops at MENU_DEFERRED_AT_A598. Therefore do NOT wait for the
         * renderer. Let the original Game Over cleanup at 0x80019FF0 finish
         * naturally, then stage hidden slot 8 from block A5A0 immediately before
         * the original menu init call. If the slot load cannot be applied, the
         * untouched original menu path remains the fallback. */
        g_return_to_menu_pending = false;
        g_gameover_menu_deferred = true;
        g_gameover_menu_native_fallback = true;
        g_gameover_restart_deferred = false;
        gameover_debug("MENU_DEFERRED_TO_POSTCLEANUP_A5A0 slot=%d", kMenuStateSlot);
        return MICKEY_GAMEOVER_MAIN_MENU;
    }

    gameover_debug("RECOVERY_NONE");
    return MICKEY_GAMEOVER_NONE;
}

/* MICKEY_GAMEOVER_V43_PRESENT_SAFELOAD
 * Called by the host frame tick after the renderer present has returned.
 * Return: 0=nothing, 1=request accepted/waiting, -1=failed. */
extern "C" int mickey_profiles_gameover_deferred_load_tick(void) {
    if (!g_gameover_restart_deferred && !g_gameover_menu_deferred)
        return 0;

    if (!g_configured || g_practice) {
        gameover_debug("PRESENT_LOAD_ABORT configured=%d practice=%d",
                       g_configured ? 1 : 0, g_practice ? 1 : 0);
        g_gameover_restart_deferred = false;
        g_gameover_menu_deferred = false;
        g_gameover_restart_pending = false;
        g_return_to_menu_pending = false;
        g_continue_pending = false;
        return -1;
    }

    if (g_gameover_restart_deferred) {
        if (!valid_profile(g_active_profile)) {
            gameover_debug("RESTART_PRESENT_LOAD_ABORT invalid_profile=%d", g_active_profile);
            g_gameover_restart_deferred = false;
            g_gameover_restart_pending = false;
            g_continue_pending = false;
            return -1;
        }

        const int slot = state_slot(g_active_profile);
        char why[128]{};
        if (!savestate_slot_exists(slot) ||
            !savestate_slot_compatible(slot, why, sizeof(why))) {
            gameover_debug("RESTART_PRESENT_LOAD_ABORT slot=%d reason=%s",
                           slot, why[0] ? why : "missing");
            g_gameover_restart_deferred = false;
            g_gameover_restart_pending = false;
            g_continue_pending = false;
            return -1;
        }

        /* One-shot: clear the queue before submitting so a re-entrant renderer
         * cannot submit the same load twice. */
        g_gameover_restart_deferred = false;
        g_continue_pending = true;
        const int accepted = savestate_request_load(slot);
        gameover_debug("RESTART_PRESENT_LOAD_REQUEST slot=%d accepted=%d", slot, accepted);
        if (!accepted) {
            g_gameover_restart_pending = false;
            g_continue_pending = false;
            return -1;
        }
        return 1;
    }

    return 0;
}

extern "C" int mickey_profiles_gameover_deferred_load_waiting(void) {
    return (g_gameover_restart_deferred || g_gameover_menu_deferred ||
            g_gameover_restart_pending || g_return_to_menu_pending) ? 1 : 0;
}

/* MICKEY_GAMEOVER_V44_MENU_POSTCLEANUP
 * Called from the generated block 0x8001A5A0 AFTER the original Game Over
 * cleanup call at 0x80019FF0 has returned, but BEFORE the original menu-init
 * call at 0x80016498. This avoids waiting for a present frame that never comes.
 *
 * Return: 0=nothing, 1=slot8 load staged, -1=hidden load unavailable/refused.
 * On -1 the generated guest code continues through its ORIGINAL menu path. */
extern "C" int mickey_profiles_gameover_menu_postcleanup_load(void) {
    if (!g_gameover_menu_deferred)
        return 0;

    g_gameover_menu_deferred = false;

    if (!g_configured || g_practice) {
        gameover_debug("MENU_POSTCLEANUP_ABORT configured=%d practice=%d",
                       g_configured ? 1 : 0, g_practice ? 1 : 0);
        g_return_to_menu_pending = false;
        return -1;
    }

    char why[128]{};
    if (!savestate_slot_exists(kMenuStateSlot) ||
        !savestate_slot_compatible(kMenuStateSlot, why, sizeof(why))) {
        gameover_debug("MENU_POSTCLEANUP_SLOT_UNAVAILABLE slot=%d reason=%s -> ORIGINAL_MENU_FALLBACK",
                       kMenuStateSlot, why[0] ? why : "missing");
        g_return_to_menu_pending = false;
        return -1;
    }

    g_return_to_menu_pending = true;
    const int accepted = savestate_request_load(kMenuStateSlot);
    gameover_debug("MENU_POSTCLEANUP_LOAD_REQUEST slot=%d accepted=%d",
                   kMenuStateSlot, accepted);
    if (!accepted) {
        g_return_to_menu_pending = false;
        gameover_debug("MENU_POSTCLEANUP_LOAD_REFUSED -> ORIGINAL_MENU_FALLBACK");
        return -1;
    }
    return 1;
}

/* If slot 8 cannot be applied, the untouched original game continues through
 * 0x80016498. Reaching A5A8 means that menu-init call returned normally, so it
 * is now safe to commit the same profile-reset semantics as v2.3.
 *
 * On a successful slot8 load the savestate callback clears this flag before
 * the old A5A8 can run, so this fallback is a no-op. */
extern "C" void mickey_profiles_gameover_menu_native_commit(void) {
    if (!g_gameover_menu_native_fallback)
        return;

    g_gameover_menu_native_fallback = false;
    g_return_to_menu_pending = false;

    if (valid_profile(g_active_profile))
        clear_profile_files(g_active_profile);

    g_new_game_pending = true;
    g_continue_pending = false;
    g_practice = false;
    g_last_story_world = -1;
    g_current_world = -1;
    g_initial_lives = -1;
    g_save_pending = false;
    g_gameover_pending = false;
    g_gameover_restart_pending = false;
    g_gameover_restart_deferred = false;
    g_gameover_resave_pending = false;

    gameover_debug("MENU_NATIVE_FALLBACK_COMMIT_AT_A5A8 profile=%d",
                   valid_profile(g_active_profile) ? g_active_profile + 1 : 0);
    std::fprintf(stdout,
        "mickey_profiles: Game Over -> original menu fallback completed; profile %d reset\n",
        valid_profile(g_active_profile) ? g_active_profile + 1 : 0);
    std::fflush(stdout);
}

extern "C" void mickey_profiles_on_savestate_configured(void) {
    g_configured = true;

    for (int i = 0; i < kProfileCount; ++i) {
        if (g_delete_pending[i]) {
            remove_state_files_now(i);
            g_delete_pending[i] = false;
        }
    }

    if (!g_continue_pending || !valid_profile(g_active_profile))
        return;

    const int p = g_active_profile;
    migrate_persisted_state_if_needed(p);

    char why[128]{};
    if (!savestate_slot_exists(state_slot(p)) ||
        !savestate_slot_compatible(state_slot(p), why, sizeof(why))) {
        std::fprintf(stderr,
            "mickey_profiles: CONTINUE profile %d unavailable/incompatible: %s\n",
            p + 1, why[0] ? why : "missing");
        g_continue_pending = false;
        return;
    }

    if (!savestate_request_load(state_slot(p))) {
        std::fprintf(stderr,
            "mickey_profiles: CONTINUE profile %d load request refused\n", p + 1);
        g_continue_pending = false;
    } else {
        std::fprintf(stdout,
            "mickey_profiles: CONTINUE load staged for profile %d\n", p + 1);
        std::fflush(stdout);
    }
}

extern "C" void mickey_profiles_on_savestate_result(int is_load, int slot, int ok) {
    /* MICKEY_PAUSE_EXIT_V45_PRESENT_SAFELOAD:
     * hidden slot 8 is only a guest-state bridge when leaving gameplay.
     * Do not deserialize RetroAchievements progress from that menu snapshot. */
    if (!(is_load && slot == kMenuStateSlot && g_return_to_frontend_pending)) {
        mickey_ra_on_savestate_result(is_load, slot, ok); /* MICKEY_RA_I18N_V1 */
    } else {
        gameover_debug("PAUSE_FRONTEND_SKIP_RA_SLOT8");
    }
    /* Slot 8 is our hidden copy of the ORIGINAL game's main menu. */
    if (slot == kMenuStateSlot) {
        if (!is_load) {
            g_menu_snapshot_pending = false;
            gameover_debug("MENU_CHECKPOINT_SAVE_RESULT ok=%d", ok);
            if (ok) {
                std::fprintf(stdout,
                    "mickey_profiles: original main-menu checkpoint ready (slot %d)\n",
                    kMenuStateSlot);
                std::fflush(stdout);
            }
        } else if (g_return_to_frontend_pending) {
            /* Pause -> WYJSCIE Z GRY: zachowaj Story save. */
            g_return_to_frontend_pending = false;
            g_pause_frontend_load_deferred = false;
            g_frontend_return_result = ok ? 1 : -1;
            gameover_debug("PAUSE_FRONTEND_LOAD_RESULT ok=%d", ok);
            if (ok) {
                g_continue_pending = false;
                g_new_game_pending = false;
                g_practice = false;
                g_last_story_world = -1;
                g_current_world = -1;
                g_initial_lives = -1;
                g_save_pending = false;
                g_gameover_pending = false;
                g_gameover_restart_pending = false;
                g_gameover_resave_pending = false;
                std::fprintf(stdout,
                    "MICKEY_UIFIX_V3_PAUSE_EXIT: hidden menu restored; Story profile preserved\n");
                std::fflush(stdout);
            }
        } else if (g_return_to_menu_pending) {
            /* Game Over -> MENU GLOWNE: user's v2.3 SafeLoad. */
            g_return_to_menu_pending = false;
            gameover_debug("MENU_LOAD_RESULT ok=%d", ok);
            if (ok) {
                g_gameover_menu_native_fallback = false;
                if (valid_profile(g_active_profile))
                    clear_profile_files(g_active_profile);
                g_new_game_pending = true;
                g_continue_pending = false;
                g_practice = false;
                g_last_story_world = -1;
                g_current_world = -1;
                g_initial_lives = -1;
                g_save_pending = false;
                g_gameover_restart_pending = false;
                g_gameover_resave_pending = false;
                std::fprintf(stdout,
                    "mickey_profiles: returned to original in-game menu; profile %d reset and armed for NEW GAME\n",
                    valid_profile(g_active_profile) ? g_active_profile + 1 : 0);
                std::fflush(stdout);
            }
        }
        return;
    }

    const int profile = slot - kStateSlotBase;
    if (!valid_profile(profile)) return;

    if (is_load) {
        gameover_debug("PROFILE_LOAD_RESULT profile=%d slot=%d ok=%d restart_pending=%d",
                       profile + 1, slot, ok, g_gameover_restart_pending ? 1 : 0);
        if (profile == g_active_profile) {
            g_continue_pending = false;
            if (ok) {
                load_one(profile);
                g_last_story_world = g_profiles[profile].world_id;
                g_current_world = g_last_story_world;
                if (g_profiles[profile].initial_lives > 0)
                    g_initial_lives = g_profiles[profile].initial_lives;

                if (g_gameover_restart_pending) {
                    g_gameover_restart_pending = false;

                    /* The checkpoint is the FIRST frame of this cartoon.
                     * Restore the player's original starting-life option and
                     * exactly 10 marbles, then immediately re-save the checkpoint
                     * so a later Continue has the same clean restart state. */
                    const int lives = g_initial_lives > 0 ? g_initial_lives : 3;
                    write_guest_u16(kLivesAddr, (uint16_t)lives);
                    write_guest_u16(kMarblesAddr, 10u);

                    g_save_world = g_current_world;
                    g_save_pending = true;
                    g_gameover_resave_pending = true;
                    g_save_visible_until = std::chrono::steady_clock::now() +
                                           std::chrono::milliseconds(1200);

                    const int resave_ok = savestate_request_save(state_slot(profile));
                    gameover_debug("RESTART_RESAVE_REQUEST slot=%d accepted=%d lives=%d marbles=10",
                                   state_slot(profile), resave_ok, lives);
                    if (!resave_ok) {
                        g_save_pending = false;
                        g_gameover_resave_pending = false;
                        std::fprintf(stderr,
                            "mickey_profiles: Game Over restart resave refused\n");
                    } else {
                        std::fprintf(stdout,
                            "mickey_profiles: Game Over restart -> %s, lives=%d, marbles=10\n",
                            valid_world(g_current_world) ? kWorldNames[g_current_world] : "UNKNOWN",
                            lives);
                        std::fflush(stdout);
                    }
                }
            } else {
                /* PROFILE_LOAD_FAILED_CLEAR_GAMEOVER_WAIT */
                if (g_gameover_restart_pending) {
                    gameover_debug("RESTART_PRESENT_LOAD_RESULT ok=0 -> clear wait");
                    g_gameover_restart_pending = false;
                }
                g_gameover_restart_deferred = false;
            }
        }
        return;
    }

    if (profile != g_active_profile || !g_save_pending)
        return;

    if (ok && !g_practice && valid_world(g_save_world))
        persist_profile(profile, g_save_world);

    if (!ok)
        std::fprintf(stderr, "mickey_profiles: autosave FAILED for profile %d\n",
                     profile + 1);

    g_save_pending = false;
    g_gameover_resave_pending = false;
}

extern "C" int mickey_profiles_pause_exit_to_frontend(void) {
    /* MICKEY_PAUSE_EXIT_V45_PRESENT_SAFELOAD
     * The pause modal only queues a return. Actual slot 8 LOAD is submitted
     * after the modal and renderer present have fully unwound. */
    if (!mickey_profiles_pause_can_exit_to_frontend())
        return 0;

    g_return_to_menu_pending = false;
    g_return_to_frontend_pending = true;
    g_pause_frontend_load_deferred = true;
    g_frontend_return_result = 0;

    gameover_debug("PAUSE_FRONTEND_DEFERRED slot=%d", kMenuStateSlot);
    return 1;
}

extern "C" int mickey_profiles_pause_can_exit_to_frontend(void) {
    if (!g_configured || g_return_to_frontend_pending)
        return 0;
    char why[128]{};
    const bool available=savestate_slot_exists(kMenuStateSlot) &&
        savestate_slot_compatible(kMenuStateSlot, why, sizeof(why));
    if (!available)
        gameover_debug("PAUSE_FRONTEND_MENU_UNAVAILABLE reason=%s",
                       why[0] ? why : "missing");
    return available ? 1 : 0;
}

extern "C" int mickey_profiles_pause_frontend_deferred_load_tick(void) {
    if (!g_return_to_frontend_pending)
        return 0;

    if (!g_pause_frontend_load_deferred)
        return 0;

    g_pause_frontend_load_deferred = false;

    const int accepted = savestate_request_load(kMenuStateSlot);
    gameover_debug("PAUSE_FRONTEND_PRESENT_LOAD_REQUEST slot=%d accepted=%d",
                   kMenuStateSlot, accepted);

    if (!accepted) {
        g_return_to_frontend_pending = false;
        g_frontend_return_result = -1;
        gameover_debug("PAUSE_FRONTEND_PRESENT_LOAD_REFUSED");
        return -1;
    }

    return 1;
}

extern "C" int mickey_profiles_pause_frontend_deferred_waiting(void) {
    return (g_return_to_frontend_pending || g_pause_frontend_load_deferred) ? 1 : 0;
}

extern "C" int mickey_profiles_take_frontend_return_result(void) {
    const int result = g_frontend_return_result;
    if (result != 0)
        g_frontend_return_result = 0;
    return result;
}

extern "C" int mickey_profiles_frontend_load_pending(void) {
    return g_continue_pending ? 1 : 0;
}

extern "C" int mickey_profiles_pause_available(void) {
    /* Pause is presentation-side and may also be used in Practice. It is only
     * exposed after a known gameplay world has actually been entered. */
    if (!valid_world(g_current_world))
        return 0;
    if (g_continue_pending || g_save_pending)
        return 0;
    if (g_gameover_pending || g_gameover_restart_pending ||
        g_gameover_restart_deferred || g_gameover_menu_deferred ||
        g_return_to_menu_pending || g_return_to_frontend_pending ||
        g_pause_frontend_load_deferred) return 0;
    return 1;
}

extern "C" int mickey_profiles_pause_can_restart(void) {
    /* Practice is intentionally NO-SAVE, therefore there is no persistent
     * checkpoint that the pause menu is allowed to load. */
    if (g_practice || !g_configured || !valid_profile(g_active_profile) ||
        !valid_world(g_current_world) || g_continue_pending || g_save_pending)
        return 0;

    /* Never load a checkpoint belonging to a different cartoon. */
    if (!g_profiles[g_active_profile].exists ||
        g_profiles[g_active_profile].world_id != g_current_world)
        return 0;

    char why[128]{};
    return savestate_slot_exists(state_slot(g_active_profile)) &&
           savestate_slot_compatible(state_slot(g_active_profile),
                                     why,sizeof(why)) ? 1 : 0;
}

extern "C" int mickey_profiles_pause_restart(void) {
    if (!mickey_profiles_pause_can_restart())
        return 0;

    /* IMPORTANT: this is a pure LOAD. We do not save the current frame and do
     * not rewrite metadata, so all progress made after the level checkpoint is
     * intentionally discarded. */
    g_continue_pending=true;
    if (!savestate_request_load(state_slot(g_active_profile))) {
        g_continue_pending=false;
        return 0;
    }

    std::fprintf(stdout,
        "MICKEY_PAUSE_MENU_V1: restart -> loading checkpoint for %s (profile %d)\n",
        valid_world(g_current_world) ? kWorldNames[g_current_world] : "UNKNOWN",
        g_active_profile+1);
    std::fflush(stdout);
    return 1;
}

extern "C" const char *mickey_profiles_pause_world_name(void) {
    return valid_world(g_current_world)
         ? kWorldNames[g_current_world]
         : "AKTUALNY POZIOM";
}

extern "C" int mickey_profiles_saving_active(void) {
    if (g_save_pending) return 1;
    return std::chrono::steady_clock::now() < g_save_visible_until ? 1 : 0;
}

/* MICKEY_NEWGAME_TO_ORIGINAL_TITLE_V2 ------------------------------------ */
extern "C" int mickey_profiles_title_boot_mask_active(void) {
    return g_new_game_title_boot_mask ? 1 : 0;
}

extern "C" void mickey_profiles_original_title_reached(void) {
    if (!g_new_game_title_boot_mask)
        return;

    g_new_game_title_boot_mask = false;
    std::fprintf(stdout,
        "MICKEY_NEWGAME_TO_ORIGINAL_TITLE_V2: original title reached -> reveal guest\n");
    std::fflush(stdout);
}


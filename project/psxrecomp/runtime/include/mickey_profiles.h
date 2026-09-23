#ifndef MICKEY_PROFILES_H
#define MICKEY_PROFILES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MickeyProfileInfo {
    int exists;
    int world_id;
    int lives;
    int marbles;
    int64_t timestamp;
    char world_name[64];
    char date_text[32];
} MickeyProfileInfo;

/* Called by the host frontend before emulation starts. */
void mickey_profiles_set_base_dir(const char *base_dir);

/* UI-facing 3-profile API (profile is 0..2). */
void mickey_profiles_select_new(int profile);
int  mickey_profiles_select_continue(int profile);
void mickey_profiles_delete(int profile);
int  mickey_profiles_get_info(int profile, MickeyProfileInfo *out);
int  mickey_profiles_active_profile(void);

/* Game-specific hooks injected into the recompiled Mickey code. */
void mickey_profiles_set_practice(int enabled);
int  mickey_profiles_is_practice(void);
void mickey_profiles_world_enter(int world_id);

/* Original PS1 main menu + Game Over recovery hooks. */
void mickey_profiles_main_menu_enter(void);
/* Game Over hook only arms recovery and ALWAYS returns 0, so the original
 * PS1 Game Over animation/music runs to completion. */
int  mickey_profiles_game_over(void);
int  mickey_profiles_game_over_recovery_point(void);

/* Submit Game Over Restart after the renderer has fully presented, never A598. */
int  mickey_profiles_gameover_deferred_load_tick(void);
int  mickey_profiles_gameover_deferred_load_waiting(void);

/* v4.4 MENU path: after original Game Over cleanup, before original menu init. */
int  mickey_profiles_gameover_menu_postcleanup_load(void);
void mickey_profiles_gameover_menu_native_commit(void);

/* Hard guard used by savestate_request_save(). In Practice this is false. */
int  mickey_profiles_save_allowed(void);

/* Hooks from psxrecomp's savestate layer. */
void mickey_profiles_on_savestate_configured(void);
void mickey_profiles_on_savestate_result(int is_load, int slot, int ok);



/* UI Fix v3: same-process Pause -> PC frontend handoff. The hidden PS1 menu
 * state is restored first; current Story profile files are never deleted. */
int  mickey_profiles_pause_exit_to_frontend(void);
int  mickey_profiles_pause_can_exit_to_frontend(void);
int  mickey_profiles_take_frontend_return_result(void);
int  mickey_profiles_frontend_load_pending(void);

/* Pause modal only queues Exit->Menu. Slot 8 LOAD is submitted by the host
 * frame tick after the pause overlay and renderer have completely unwound. */
int  mickey_profiles_pause_frontend_deferred_load_tick(void);
int  mickey_profiles_pause_frontend_deferred_waiting(void);

/* Host pause menu. Restart is a PURE LOAD of the existing Story checkpoint:
 * it never saves the current progress. Practice can pause but cannot restart. */
int  mickey_profiles_pause_available(void);
int  mickey_profiles_pause_can_restart(void);
int  mickey_profiles_pause_restart(void);
const char *mickey_profiles_pause_world_name(void);

/* Host overlay state. */
int  mickey_profiles_saving_active(void);


/* MICKEY_NEWGAME_TO_ORIGINAL_TITLE_V2 */
int  mickey_profiles_title_boot_mask_active(void);
void mickey_profiles_original_title_reached(void);
#ifdef __cplusplus
}
#endif

#endif

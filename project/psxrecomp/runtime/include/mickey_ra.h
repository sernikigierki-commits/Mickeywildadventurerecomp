#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MickeyRAAchievement { uint32_t id, points; int unlocked; char title[256]; char description[768]; char badge_url[512]; char badge_locked_url[512]; } MickeyRAAchievement;
typedef struct MickeyRAUnlockEvent { uint32_t id, points; char title[256]; char description[768]; char badge_url[512]; } MickeyRAUnlockEvent;
void mickey_ra_init(const char* base_dir);
void mickey_ra_set_disc_path(const char* path);
void mickey_ra_shutdown(void);
void mickey_ra_do_frame(void);
void mickey_ra_idle(void);
void mickey_ra_reset_runtime(void);
void mickey_ra_on_savestate_result(int is_load, int slot, int ok);
int mickey_ra_logged_in(void);
int mickey_ra_game_loaded(void);
const char* mickey_ra_username(void);
const char* mickey_ra_status(void);
int mickey_ra_achievement_count(void);
int mickey_ra_get_achievement(int index, MickeyRAAchievement* out);
int mickey_ra_pop_unlock_event(MickeyRAUnlockEvent* out);
int mickey_ra_login_prompt(int language);
int mickey_ra_login_with_password(const char* username,const char* password);
int mickey_ra_login_pending(void);
void mickey_ra_logout(void);
#ifdef __cplusplus
}
#endif

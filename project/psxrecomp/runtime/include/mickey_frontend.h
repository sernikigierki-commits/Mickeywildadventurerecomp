#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

enum {
    MICKEY_GAMEOVER_NONE = 0,
    MICKEY_GAMEOVER_RESTART = 1,
    MICKEY_GAMEOVER_MAIN_MENU = 2
};

/* Story-only Game Over recovery UI. PRACTICE never calls this path. */
int mickey_frontend_game_over_dialog(const char *world_name,
                                     int initial_lives,
                                     int can_restart,
                                     int can_main_menu);

/* Runs once, inside psxrecomp's already-created OpenGL window/context.
 * Blocks until START GRY. WYJSCIE terminates the process.
 */
void mickey_frontend_run(struct SDL_Window *window);
int mickey_frontend_initialized(void);

/* Called immediately before SDL_GL_SwapWindow by the OpenGL backend.
 * Draws the one-shot circular opening transition over the first game frames.
 */
void mickey_frontend_draw_game_transition(void);

/* Commit modal actions at the host vblank boundary, after renderer present
 * and SDL_GL_SwapWindow have returned. Never call from a draw/event callback. */
void mickey_frontend_host_frame_tick(void);


/* Called by the offline pad sampler before the PS1 sees Player 1 input.
 * START is active-low bit 3. During gameplay it becomes the host Pause key. */
uint16_t mickey_frontend_pause_filter_buttons(uint16_t buttons);

#ifdef __cplusplus
}
#endif

#include "cpu_state.h"
#include "interrupts.h"
#include "parity_trace.h"
#include "psx_cycles.h"
#include "psx_selfcheck.h"

#include <string.h>
#ifdef MICKEY_ANDROID_GAMEPLAY_PROFILE
#include <android/log.h>
#endif

extern int g_precise_mode, g_ls_mode, g_psx_call_bail;
extern volatile int g_ds_recording;
extern uint64_t g_guest_store_count;
extern uint32_t g_debug_last_store_pc;
extern int psx_netplay_active(void);

static CPUState s_previous_cpu;
static uint32_t s_previous_count;
static uint32_t s_previous_clock;
static uint64_t s_previous_cycle;
static int s_previous_valid;

void mickey_armv7_wait_accel(CPUState *cpu) {
    /* This stack and return address identify the verified Steamboat Willie
     * VBlank wait. Other scenes and frontend/startup execution stay untouched. */
    if (cpu->gpr[29] != 0x801FFE28u || cpu->gpr[31] != 0x8007FD80u ||
        cpu->gpr[2] != 1u || cpu->gpr[3] != UINT32_MAX ||
        g_precise_mode || g_ls_mode || g_ls_replay_active || g_ds_recording ||
        g_event_step_conservative || g_psx_call_bail || g_idle_skip_enabled != 0 ||
        psx_get_in_exception() || psx_netplay_active() || psx_selfcheck_enabled() ||
        parity_trace_is_armed() || g_psx_cyc_batch || g_psx_cyc_local_acc ||
        (cpu->cop0[12] & 0x10000u)) {
        s_previous_valid = 0;
        return;
    }

    uint32_t count = cpu->read_word(cpu->gpr[29] + 16u);
    uint32_t clock = cpu->read_word(0x800AEE1Cu);
    uint64_t cycle = psx_get_cycle_count();
    if (count < 64u || count > 32768u || clock + 1u != cpu->gpr[4]) {
        s_previous_valid = 0;
        return;
    }

    int stable = s_previous_valid && s_previous_count == count + 1u &&
        s_previous_clock == clock && cycle == s_previous_cycle + 32u &&
        memcmp(&s_previous_cpu, cpu, sizeof *cpu) == 0;
    memcpy(&s_previous_cpu, cpu, sizeof *cpu);
    s_previous_count = count;
    s_previous_clock = clock;
    s_previous_cycle = cycle;
    s_previous_valid = 1;
    if (!stable) return;

    uint32_t distance = psx_idle_cycles_to_next_observable_event();
    if (distance <= 128u) return;
    uint32_t iterations = (distance - 64u) / 32u;
    if (iterations >= count) iterations = count - 1u;
    if (iterations < 64u) return;

    g_debug_last_store_pc = 0x8007FE40u;
    cpu->write_word(cpu->gpr[29] + 16u, count - iterations);
    g_guest_store_count += iterations - 1u;
    psx_advance_cycles(iterations * 32u);
    s_previous_valid = 0;
#ifdef MICKEY_ANDROID_GAMEPLAY_PROFILE
    {
        static unsigned skips;
        if (((++skips) & 63u) == 1u)
            __android_log_print(ANDROID_LOG_INFO, "MickeyWaitAccel",
                "skips=%u iterations=%u cycles=%u remaining=%u",
                skips, iterations, iterations * 32u, count - iterations);
    }
#endif
}

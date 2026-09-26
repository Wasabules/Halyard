/* rumble_state.c - see rumble_state.h. */

#include "rumble_state.h"

#include <pthread.h>

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t  g_basse = 0, g_haute = 0;
static bool     g_change = false;
static uint32_t g_recus = 0;

void rumble_state_set(uint8_t id, uint8_t basse, uint8_t haute)
{
    (void)id;   /* read and carried, never a reason to reject - see gamepad_wire.h */
    pthread_mutex_lock(&g_mtx);
    g_recus++;
    if (basse != g_basse || haute != g_haute) {
        g_basse = basse;
        g_haute = haute;
        g_change = true;
    }
    pthread_mutex_unlock(&g_mtx);
}

bool rumble_state_get(uint8_t *basse, uint8_t *haute)
{
    pthread_mutex_lock(&g_mtx);
    const bool chg = g_change;
    g_change = false;
    if (basse) *basse = g_basse;
    if (haute) *haute = g_haute;
    pthread_mutex_unlock(&g_mtx);
    return chg;
}

void rumble_state_stop(void)
{
    pthread_mutex_lock(&g_mtx);
    /* Force `g_change` EVEN when the amplitudes were already zero: the caller
     * must be able to shut down a motor the hardware is still driving, for
     * instance one left running by a previous session. Without this, a shutdown
     * requested on an already-zero state would produce no call at all. */
    g_basse = 0;
    g_haute = 0;
    g_change = true;
    g_recus = 0;
    pthread_mutex_unlock(&g_mtx);
}

uint32_t rumble_state_recus(void)
{
    pthread_mutex_lock(&g_mtx);
    const uint32_t n = g_recus;
    pthread_mutex_unlock(&g_mtx);
    return n;
}

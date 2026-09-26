/* native_input.c - see header. */

#include "native_input.h"
#include "ctrl_input_tcp.h"
#include "../common/log.h"

/* S81 - this module's journal category. See shadow/journal.h: it is declared
 * here, never inferred from the text of the messages. */
#define nilog(...) JOURNAL_INFO_(JOURNAL_CAT_INPUT, __VA_ARGS__)
#define nidbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_INPUT, __VA_ARGS__)

#include <pthread.h>
#include <stdatomic.h>

/* Singleton - Shadow runs one session at a time, so static globals are fine. */
static ctrl_input_tcp_t *g_itc = NULL;
static pthread_mutex_t   g_mtx = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int       g_active = 0;  /* lock-free read for the hot path */

void native_input_set(ctrl_input_tcp_t *itc) {
    pthread_mutex_lock(&g_mtx);
    g_itc = itc;
    atomic_store_explicit(&g_active, itc ? 1 : 0, memory_order_release);
    pthread_mutex_unlock(&g_mtx);
    nilog("[native_input] set itc=%p (active=%d)", (void *)itc, itc ? 1 : 0);
}

bool native_input_active(void) {
    return atomic_load_explicit(&g_active, memory_order_acquire) != 0;
}

/* Diagnostic counters - logged every 100 events so they do not flood the log */
static int g_n_moves = 0, g_n_btns = 0, g_n_keys = 0, g_n_wheels = 0;

int native_input_send_mouse_move(int dx, int dy) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (g_itc) rc = ctrl_input_tcp_send_mouse_move(g_itc, dx, dy);
    g_n_moves++;
    if (g_n_moves <= 5 || g_n_moves % 100 == 0)
        nilog("[native_input] mouse_move #%d dx=%d dy=%d rc=%d", g_n_moves, dx, dy, rc);
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

/* C3 - ABSOLUTE variant, cf. ctrl_input_tcp.c */
int native_input_send_mouse_move_abs(int x, int y) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (g_itc) rc = ctrl_input_tcp_send_mouse_move_abs(g_itc, x, y);
    g_n_moves++;
    if (g_n_moves <= 5 || g_n_moves % 100 == 0)
        nilog("[native_input] mouse_move #%d dx=%d dy=%d rc=%d", g_n_moves, x, y, rc);
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int native_input_send_mouse_button(int button, bool pressed) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (g_itc) rc = ctrl_input_tcp_send_mouse_button(g_itc, button, pressed);
    g_n_btns++;
    nilog("[native_input] mouse_btn #%d btn=%d pressed=%d rc=%d", g_n_btns, button, pressed, rc);
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int native_input_send_scancode(uint16_t scancode, bool pressed) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (g_itc) rc = ctrl_input_tcp_send_scancode(g_itc, scancode, pressed);
    g_n_keys++;
    nilog("[native_input] scancode #%d code=%u pressed=%d rc=%d", g_n_keys, scancode, pressed, rc);
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int native_input_send_mouse_wheel(int direction) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (g_itc) rc = ctrl_input_tcp_send_mouse_wheel(g_itc, direction);
    g_n_wheels++;
    nilog("[native_input] wheel #%d dir=%d rc=%d", g_n_wheels, direction, rc);
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

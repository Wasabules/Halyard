/* virtual_gamepad — creates a uinput gamepad for the duration of a test, then exits.
 *
 * Why: `ctrl_gamepad_present()` classifies through sysfs — it needs at least one
 * key bit in [BTN_GAMEPAD .. BTN_THUMBR] AND the ABS_X/ABS_Y axes
 * (ctrl_gamepad.c::sysfs_is_gamepad). With no physical gamepad on the machine, neither
 * G52 (the plug-in announcement) nor G53 (the server's reply) can be
 * exercised: the reader does not even start.
 *
 * The name avoids "Motion Sensor" and "halyard-virtual", which the detector
 * ecarte explicitement.
 *
 * Usage: virtual_gamepad <seconds>   (it wiggles the axes for that long)
 */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void emit(int fd, int type, int code, int val)
{
    struct input_event ev = {0};
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = val;
    if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev) { /* non fatal */ }
}

int main(int argc, char **argv)
{
    const int secondes = (argc > 1) ? atoi(argv[1]) : 30;

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); return 1; }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int b = BTN_GAMEPAD; b <= BTN_THUMBR; b++) ioctl(fd, UI_SET_KEYBIT, b);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int a = ABS_X; a <= ABS_RZ; a++) ioctl(fd, UI_SET_ABSBIT, a);

    struct uinput_abs_setup abs = {0};
    abs.absinfo.minimum = -32768;
    abs.absinfo.maximum =  32767;
    abs.absinfo.flat = 128; abs.absinfo.fuzz = 16;
    for (int a = ABS_X; a <= ABS_RZ; a++) {
        abs.code = (unsigned short)a;
        ioctl(fd, UI_ABS_SETUP, &abs);
    }

    struct uinput_setup us = {0};
    us.id.bustype = BUS_USB;
    us.id.vendor  = 0x054c;   /* Sony, to resemble the DualShock in the captures */
    us.id.product = 0x09cc;
    snprintf(us.name, sizeof us.name, "Manette de test (uinput)");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); return 1; }
    if (ioctl(fd, UI_DEV_CREATE) < 0)     { perror("UI_DEV_CREATE"); return 1; }

    fprintf(stderr, "[manette] creee, %d s\n", secondes);
    fflush(stderr);

    /* Wiggle it: the evdev reader must have something to transmit, otherwise
     * we only test its presence and not the complete path. */
    const struct timespec pause = {0, 50 * 1000 * 1000};   /* 50 ms */
    for (int i = 0; i < secondes * 20; i++) {
        const int v = (int)(30000.0 * ((i % 40) - 20) / 20.0);
        emit(fd, EV_ABS, ABS_X, v);
        emit(fd, EV_ABS, ABS_Y, -v);
        if ((i % 20) == 0) emit(fd, EV_KEY, BTN_SOUTH, 1);
        if ((i % 20) == 5) emit(fd, EV_KEY, BTN_SOUTH, 0);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        nanosleep(&pause, NULL);
    }

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    fprintf(stderr, "[manette] detruite\n");
    return 0;
}

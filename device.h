#ifndef DEVICE_H
#define DEVICE_H
#include <stddef.h>     /* size_t, for device_macrocell_ball() */

typedef struct {
    const char *name;      /* "XC7336" */
    int n_fbs;             /* function blocks */
    int n_mcs;             /* total macrocells */
    int ffb_mcs;           /* macrocells per fast function block */
    long qf;               /* JEDEC fuse count, 0 = NOT KNOWN */
    int  qf_measured;      /* 1 = measured from a real bitmap, 0 = unknown */
} device_t;

int device_ball_macrocell(const device_t *dev, const char *package,
                          const char *ball, int *fb, int *mc);
/* The REVERSE of device_ball_macrocell, same table.
 * PG144/BG225 have no numeric pin at all -- every macrocell's OWN package
 * position is an alphanumeric ball label, so `decode.c`'s numeric
 * pin_for_macrocell()/device_pin_macrocell() path (which needs a real
 * package pin NUMBER to search over) can never resolve one for these two
 * packages. Returns 1 and fills `out` (NUL-terminated) if (fb,mc) has a
 * ball on this device/package, 0 otherwise. */
int device_macrocell_ball(const device_t *dev, const char *package,
                          int fb, int mc, char *out, size_t outsz);
const device_t *device_find(const char *name);
const device_t *device_nth(int i);
int device_count(void);

/* XC7336/PC44 only so far. Returns 1 and fills fb/mc if the pin drives a
 * macrocell, 0 otherwise. */
int device_pin_macrocell(const device_t *dev, const char *package, int pin,
                         int *fb, int *mc);

/* Special function of a pin ("MR", "FCLK0", "GND", ...) or NULL. */
const char *device_pin_function(const device_t *dev, const char *package,
                                int pin);

#endif

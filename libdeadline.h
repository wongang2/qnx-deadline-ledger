/* libdeadline.h - client side of deadlined (header-only). */
#ifndef LIBDEADLINE_H
#define LIBDEADLINE_H
#include <stdint.h>
#include <devctl.h>
#include <sys/neutrino.h>

#define DL_TICK_CODE      (_PULSE_CODE_MINAVAIL + 1)
#define DL_DEADLINE_CODE  (_PULSE_CODE_MINAVAIL + 2)
#define DCMD_DEADLINE_GET_ID __DIOF(_DCMD_MISC, 0x51, int)

/* pulse value: contract id in the top 12 bits, microseconds of work in the low 20 */
#define DL_TICK_VALUE(id, used_us) ((int)((((unsigned)(id)) << 20) | ((unsigned)(used_us) & 0xFFFFFu)))
#define DL_TICK_ID(v)      ((int)(((unsigned)(v)) >> 20))
#define DL_TICK_USED_US(v) ((unsigned)(v) & 0xFFFFFu)

/* Ask the server which contract id this fd is bound to. */
static inline int dl_contract_id(int fd) {
    int id = 0;
    if (devctl(fd, DCMD_DEADLINE_GET_ID, &id, sizeof id, &id) != 0) return -1;
    return id;
}

/* One completed cycle. Non-blocking: a pulse, not a message, so the loop never waits on the server. */
static inline int dl_tick(int fd, int id, unsigned used_us, int prio) {
    return MsgSendPulse(fd, prio, DL_TICK_CODE, DL_TICK_VALUE(id, used_us));
}
#endif

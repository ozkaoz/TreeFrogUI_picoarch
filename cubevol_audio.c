/* cubevol_audio.c - stock-audio interop for SF-class handhelds.
 *
 * The console volume (cubevol, GPIO keys) never gates the speaker amp - it is
 * a digital gain BEFORE the power amp. The amp (device tree /panel/
 * speaker-output) stays powered whenever cubevol believes a speaker should be
 * active, and its idle hiss is the constant static audible even at volume 0.
 *
 * This module reads cubevol's own persisted volume so picoarch can keep the
 * speaker amp gated whenever the console is muted (volume 0): a muted console
 * has no business powering the amp at all. Struct + ioctl must match cubevol
 * exactly (avparam_get_volume): req{flag=3,id=0,len=260}, volume = buf[0]. */
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>

struct cube_pmem_req { unsigned short flag, id, len, pad; void *buf; };
#define CUBE_PMEM_GET 0x400c2602u

/* Stored system volume 0..100, or -1 if unreadable. */
int cube_vol_read(void) {
    int fd = open("/dev/persistentmem", O_RDWR);
    if (fd < 0) return -1;
    unsigned char buf[260] = {0};
    struct cube_pmem_req req = { 3, 0, 260, 0, buf };
    int rv = ioctl(fd, CUBE_PMEM_GET, &req);
    close(fd);
    if (rv < 0) return -1;
    return buf[0];
}

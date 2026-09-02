/* fn_input_diag.c — Standalone FN/button raw diagnostic for SF3000-class devices
 * Feature C1: shows RAW_PREVIOUS / RAW_CURRENT / CHANGED_MASK / BITS_PRESSED / BITS_RELEASED
 * This utility is standalone, not active by default in picoarch, and does not affect gameplay.
 * Compile: gcc -o fn_diag fn_input_diag.c  (or mips-mti-linux-gnu-gcc for target)
 * Usage: ./fn_diag   — press FN and other buttons, observe which bit flips.
 * Example conceptual output:
 *   prev=0x0000 cur=0x0002 changed=0x0002 pressed_bit=1
 * Does NOT assume bit 1 == FN; requires physical press to calibrate.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>

int main(void) {
    key_t k = ftok("/tmp/joy_key", 'a');
    if (k == (key_t)-1) {
        perror("ftok /tmp/joy_key");
        fprintf(stderr, "hint: ensure cubevol is running and /tmp/joy_key exists\n");
        return 1;
    }
    /* READ ONLY: attach with SHM_RDONLY, never IPC_CREAT — fail if real segment missing */
    int id = shmget(k, 4, 0666);
    if (id < 0) {
        perror("shmget /tmp/joy_key (cubevol not running or shm missing)");
        fprintf(stderr, "hint: ensure cubevol is running, /tmp/joy_key exists, and ftok matches picoarch\n");
        return 1;
    }
    volatile uint32_t *ptr = (volatile uint32_t*)shmat(id, NULL, SHM_RDONLY);
    if (ptr == (void*)-1) {
        perror("shmat SHM_RDONLY");
        return 1;
    }
    printf("fn_diag: READ ONLY monitoring /tmp/joy_key (full 32 bits). Press buttons including FN, Ctrl-C to exit.\n");
    printf("DIAG READ ONLY — does not write shm, does not kill cubevol/rkgame, does not modify GPIO/drivers\n");
    fflush(stdout);
    uint32_t prev = *ptr;
    /* Also handle full 32-bit raw; FN could be outside 0-15 */
    while (1) {
        usleep(10000);
        uint32_t cur = *ptr;
        uint32_t changed = prev ^ cur;
        if (changed) {
            uint32_t pressed = cur & changed;
            uint32_t released = (~cur) & changed;
            /* Required minimal output labels */
            printf("RAW_PREVIOUS=0x%08X\n", prev);
            printf("RAW_CURRENT=0x%08X\n", cur);
            printf("CHANGED_MASK=0x%08X\n", changed);
            printf("BITS_PRESSED=");
            int first=1;
            for (int b=0;b<32;b++) if (pressed & (1u<<b)) { if(!first) printf(","); printf("%d", b); first=0; }
            if(first) printf("none");
            printf("\n");
            printf("BITS_RELEASED=");
            first=1;
            for (int b=0;b<32;b++) if (released & (1u<<b)) { if(!first) printf(","); printf("%d", b); first=0; }
            if(first) printf("none");
            printf("\n");
            /* Compact single-line also for quick capture */
            printf("compact: prev=0x%08X cur=0x%08X changed=0x%08X pressed:", prev, cur, changed);
            for (int b=0;b<32;b++) if (pressed & (1u<<b)) printf(" %d", b);
            printf(" released:");
            for (int b=0;b<32;b++) if (released & (1u<<b)) printf(" %d", b);
            printf("\n---\n");
            fflush(stdout);
            /* Note: does NOT use SF3000_FN_BIT / bit 1 / bit 2 to decide; human identifies via physical press */
        }
        prev = cur;
    }
    return 0;
}

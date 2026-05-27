/* SF3000 audio system using driver.so hardware acceleration
   Targets 60Hz output with 735-sample chunks (matching stock rkgame performance) */
#ifndef SF3000_AUDIO_H
#define SF3000_AUDIO_H

#include <stdint.h>
#include <pthread.h>
#include <dlfcn.h>

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_CHANNELS 2
#define AUDIO_CHUNK_FRAMES 735  // 44100/60 = 735 frames per chunk at 60Hz
#define AUDIO_BUFFER_SIZE (AUDIO_CHUNK_FRAMES * 4)  // Ring buffer: 735*4 = 2940 frames

// Forward declaration - audio_frame is defined in plat.h
struct audio_frame;

/* driver.so function pointers */
static void (*sound_driver_init)(void) = NULL;
static void (*sound_driver_playframe)(const void *) = NULL;
static void (*sound_driver_deinit)(void) = NULL;

/* Ring buffer for audio */
static struct {
    struct audio_frame buffer[AUDIO_BUFFER_SIZE];
    unsigned write_pos;
    unsigned read_pos;
    pthread_mutex_t mutex;
    pthread_t thread;
    int running;
    int initialized;
} audio_state;

/* Background audio thread: plays 735 frames every 16.67ms (60Hz) */
static void* audio_thread_fn(void* arg) {
    const struct timespec delay = { 0, 16666667 };  // 16.67ms = 1/60s

    while (audio_state.running) {
        nanosleep(&delay, NULL);

        pthread_mutex_lock(&audio_state.mutex);

        // Check if we have enough frames (735 = 1 chunk)
        unsigned available = (audio_state.write_pos - audio_state.read_pos) % AUDIO_BUFFER_SIZE;
        if (available < AUDIO_CHUNK_FRAMES) {
            pthread_mutex_unlock(&audio_state.mutex);
            continue;  // Underrun - wait for more data
        }

        // Get 735 frames from ring buffer
        struct audio_frame chunk[AUDIO_CHUNK_FRAMES];
        for (int i = 0; i < AUDIO_CHUNK_FRAMES; i++) {
            chunk[i] = audio_state.buffer[audio_state.read_pos];
            audio_state.read_pos = (audio_state.read_pos + 1) % AUDIO_BUFFER_SIZE;
        }

        pthread_mutex_unlock(&audio_state.mutex);

        // Send to hardware via driver.so
        if (sound_driver_playframe) {
            sound_driver_playframe(chunk);
        }
    }

    return NULL;
}

/* Initialize audio system with driver.so hardware acceleration */
int sf3000_audio_init(void) {
    if (audio_state.initialized) return 0;

    // Load driver.so
    void* driver = dlopen("/mnt/sdcard/cubegm/driver.so", RTLD_LAZY);
    if (!driver) {
        fprintf(stderr, "Failed to load driver.so: %s\n", dlerror());
        return -1;
    }

    // Resolve audio functions
    sound_driver_init = dlsym(driver, "sound_driver_init");
    sound_driver_playframe = dlsym(driver, "sound_driver_playframe");
    sound_driver_deinit = dlsym(driver, "sound_driver_deinit");

    if (!sound_driver_playframe) {
        fprintf(stderr, "Failed to resolve sound_driver_playframe\n");
        dlclose(driver);
        return -1;
    }

    // Initialize hardware
    if (sound_driver_init) {
        sound_driver_init();
    }

    // Initialize ring buffer
    memset(&audio_state, 0, sizeof(audio_state));
    pthread_mutex_init(&audio_state.mutex, NULL);
    audio_state.running = 1;

    // Start 60Hz audio thread
    if (pthread_create(&audio_state.thread, NULL, audio_thread_fn, NULL) != 0) {
        fprintf(stderr, "Failed to create audio thread\n");
        sound_driver_deinit();
        dlclose(driver);
        return -1;
    }

    audio_state.initialized = 1;
    fprintf(stderr, "SF3000 audio: 60Hz thread + 735-frame chunks initialized\n");
    return 0;
}

/* Write audio frames to ring buffer (called by core) */
void sf3000_audio_write(const struct audio_frame *data, int frames) {
    if (!audio_state.initialized) return;

    pthread_mutex_lock(&audio_state.mutex);

    for (int i = 0; i < frames; i++) {
        unsigned next = (audio_state.write_pos + 1) % AUDIO_BUFFER_SIZE;
        if (next == audio_state.read_pos) {
            break;  // Buffer full - drop frame
        }
        audio_state.buffer[audio_state.write_pos] = data[i];
        audio_state.write_pos = next;
    }

    pthread_mutex_unlock(&audio_state.mutex);
}

#endif /* SF3000_AUDIO_H */
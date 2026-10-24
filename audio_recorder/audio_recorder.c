
#define _GNU_SOURCE

#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <sndfile.h>

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define RINGBUFFER_SIZE (16 * 1024 * 1024)
#define WRITE_FRAMES   4096

static jack_client_t *client;
static jack_port_t *input_port;
static jack_ringbuffer_t *ringbuffer;

static atomic_bool writer_stop = false;
static volatile sig_atomic_t stop_requested = 0;

static void handle_sigint(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/* JACK real-time callback: copy mono input into the ring buffer. */
static int process_audio(jack_nframes_t nframes, void *arg)
{
    (void)arg;

    jack_default_audio_sample_t *input =
        jack_port_get_buffer(input_port, nframes);

    size_t bytes = (size_t)nframes * sizeof(float);

    /* Drop this entire callback block if there isn't enough room. */
    if (jack_ringbuffer_write_space(ringbuffer) >= bytes) {
        jack_ringbuffer_write(ringbuffer, (const char *)input, bytes);
    }

    return 0;
}

/* Non-real-time thread: write captured samples to the WAV file. */
static void *writer_thread(void *arg)
{
    SNDFILE *file = (SNDFILE *)arg;
    float samples[WRITE_FRAMES];

    for (;;) {
        size_t available = jack_ringbuffer_read_space(ringbuffer);
        size_t frames = available / sizeof(float);

        if (frames > WRITE_FRAMES)
            frames = WRITE_FRAMES;

        if (frames > 0) {
            size_t bytes = frames * sizeof(float);

            jack_ringbuffer_read(
                ringbuffer, (char *)samples, bytes);

            sf_count_t written = sf_writef_float(file, samples, frames);

            if (written != (sf_count_t)frames) {
                fprintf(stderr, "Error writing WAV data: %s\n",
                        sf_strerror(file));
                atomic_store(&writer_stop, true);
                break;
            }
        } else if (atomic_load(&writer_stop)) {
            break;
        } else {
            usleep(1000);
        }
    }

    return NULL;
}

int main(int argc, char **argv)
{
    const char *source_port =
        (argc > 1) ? argv[1] : "system:capture_1";

    char filename[] = "recording-XXXXXX.wav";
    int temp_fd = mkstemps(filename, 4);

    if (temp_fd < 0) {
        perror("mkstemps");
        return 1;
    }

    close(temp_fd);

    SF_INFO sfinfo = {0};

    sfinfo.samplerate = 44100; /* Updated below from JACK. */
    sfinfo.channels = 1;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    client = jack_client_open("jack_recorder", JackNullOption, NULL);

    if (!client) {
        fprintf(stderr, "Could not connect to JACK server.\n");
        unlink(filename);
        return 1;
    }

    sfinfo.samplerate = (int)jack_get_sample_rate(client);

    input_port = jack_port_register(
        client, "input", JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsInput, 0);

    if (!input_port) {
        fprintf(stderr, "Could not register JACK input port.\n");
        jack_client_close(client);
        unlink(filename);
        return 1;
    }

    ringbuffer = jack_ringbuffer_create(RINGBUFFER_SIZE);

    if (!ringbuffer) {
        fprintf(stderr, "Could not create ring buffer.\n");
        jack_client_close(client);
        unlink(filename);
        return 1;
    }

    jack_ringbuffer_mlock(ringbuffer);

    SNDFILE *file = sf_open(filename, SFM_WRITE, &sfinfo);

    if (!file) {
        fprintf(stderr, "Could not create WAV file: %s\n",
                sf_strerror(NULL));
        jack_ringbuffer_free(ringbuffer);
        jack_client_close(client);
        unlink(filename);
        return 1;
    }

    if (jack_set_process_callback(client, process_audio, NULL) != 0) {
        fprintf(stderr, "Could not set JACK process callback.\n");
        sf_close(file);
        jack_ringbuffer_free(ringbuffer);
        jack_client_close(client);
        unlink(filename);
        return 1;
    }

    pthread_t writer;
    if (pthread_create(&writer, NULL, writer_thread, file) != 0) {
        perror("pthread_create");
        sf_close(file);
        jack_ringbuffer_free(ringbuffer);
        jack_client_close(client);
        unlink(filename);
        return 1;
    }

    if (jack_activate(client) != 0) {
        fprintf(stderr, "Could not activate JACK client.\n");
        atomic_store(&writer_stop, true);
        pthread_join(writer, NULL);
        sf_close(file);
        jack_ringbuffer_free(ringbuffer);
        jack_client_close(client);
        unlink(filename);
        return 1;
    }

    if (jack_connect(client, source_port,
                     jack_port_name(input_port)) != 0) {
        fprintf(stderr, "Could not connect %s to recorder input.\n",
                source_port);
        jack_deactivate(client);
        atomic_store(&writer_stop, true);
        pthread_join(writer, NULL);
        sf_close(file);
        jack_ringbuffer_free(ringbuffer);
        jack_client_close(client);
        unlink(filename);
        return 1;
    }

    signal(SIGINT, handle_sigint);

    printf("Recording from %s\n", source_port);
    printf("Output: %s\n", filename);
    printf("Sample rate: %d Hz, mono, 16-bit PCM WAV\n",
           sfinfo.samplerate);
    printf("Press Ctrl+C to stop.\n");

    while (!stop_requested)
        usleep(100000);

    /* Stop new audio callbacks before telling the writer to drain. */
    jack_deactivate(client);

    atomic_store(&writer_stop, true);
    pthread_join(writer, NULL);

    sf_close(file); /* Finalizes the WAV header. */

    jack_ringbuffer_free(ringbuffer);
    jack_client_close(client);

    printf("\nRecording saved: %s\n", filename);
    return 0;
}

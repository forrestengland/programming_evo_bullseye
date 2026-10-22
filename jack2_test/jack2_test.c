#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <sndfile.h>

typedef struct {
  jack_client_t *client;
  jack_port_t **ports;
  jack_ringbuffer_t *ringbuffer;
  SNDFILE *sndfile;
  SF_INFO sfinfo;
  int playing;
} int_client_t;

int_client_t app;

int jack_process_cb(jack_nframes_t nframes, void *arg) {

  int_client_t *a = (int_client_t *)arg;

  float **out_buffers = malloc(sizeof(float*) * a->sfinfo.channels);
  for (int ch=0; ch<a->sfinfo.channels; ch++) {
    out_buffers[ch] = (jack_default_audio_sample_t *)jack_port_get_buffer(a->ports[ch], nframes);
  }

  size_t samples_needed = nframes * a->sfinfo.channels;
  size_t bytes_needed = samples_needed * sizeof(float);

  if (!a->playing || jack_ringbuffer_read_space(a->ringbuffer) < bytes_needed) {
    for (int ch=0; ch < a->sfinfo.channels; ch++) {
      memset(out_buffers[ch], 0, nframes * sizeof(float));
    }
    free(out_buffers);
    return 0;
  }

  float *interleaved_chunk = malloc(bytes_needed);
  jack_ringbuffer_read(a->ringbuffer, (char *)interleaved_chunk, bytes_needed);

  for (jack_nframes_t frame=0; frame < nframes; frame++) {
    for (int ch=0; ch<a->sfinfo.channels; ch++) {
      out_buffers[ch][frame] = interleaved_chunk[frame * a->sfinfo.channels + ch];
    }
  }

  free(interleaved_chunk);
  free(out_buffers);
  return 0;
}

void *disk_reader_thread(void* arg) {

  int_client_t *a = (int_client_t*)arg;
  size_t block_samples = 1024 * a->sfinfo.channels;
  float *disk_buffer = malloc(block_samples * sizeof(float));

  while (a->playing) {
    size_t write_space_bytes = jack_ringbuffer_write_space(a->ringbuffer);
    size_t block_size_bytes = block_samples * sizeof(float);

    if (write_space_bytes >= block_size_bytes) {
      sf_count_t read_frames = sf_readf_float(a->sndfile, disk_buffer, 1024);
      if (read_frames <= 0){
	printf("end of file reached\n");
	a->playing = 0;
	break;
      }
      jack_ringbuffer_write(a->ringbuffer, (char*)disk_buffer, read_frames * a->sfinfo.channels * sizeof(float));
    } else {
      usleep(10000);
    }
  }
  free(disk_buffer);
  return NULL;
}

int main(int argc, char *argv[]) {

  memset(&app.sfinfo, 0, sizeof(SF_INFO));
  app.sndfile = sf_open("drums__10_3_2026__1.wav", SFM_READ, &app.sfinfo);
  if (!app.sndfile) {
    fprintf(stderr, "error opening wav file: %s\n", sf_strerror(NULL));
    return 1;
  }

  printf("loaded file\n");
  printf("sample rate: %d hz, channels : %d\n", app.sfinfo.samplerate, app.sfinfo.channels);

  jack_status_t status;
  app.client = jack_client_open("wavplayer", JackNullOption, &status);
  if (!app.client) {
    fprintf(stderr, "could not connect to jack\n");
    sf_close(app.sndfile);
    return 1;
  }

  app.ports = malloc(sizeof(jack_port_t*) * app.sfinfo.channels);
  for (int i=0; i<app.sfinfo.channels; i++) {
    char port_name[32];
    sprintf(port_name, "out_%d", i+1);
    app.ports[i] = jack_port_register(app.client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  }

  jack_set_process_callback(app.client, jack_process_cb, &app);

  app.ringbuffer = jack_ringbuffer_create(1024 * 1024);
  app.playing = 1;

  // activate jack
  if (jack_activate(app.client)) {
    fprintf(stderr, "could not activate jack client\n");
    return 1;
  }

  pthread_t thread_id;
  pthread_create(&thread_id, NULL, disk_reader_thread, &app);

  printf("playing wav audio. Ctrl+C to stop");
  while (app.playing) {
    usleep(100000);
  }

  pthread_join(thread_id, NULL);
  jack_client_close(app.client);
  jack_ringbuffer_free(app.ringbuffer);
  sf_close(app.sndfile);
  free(app.ports);

  return 0;
}


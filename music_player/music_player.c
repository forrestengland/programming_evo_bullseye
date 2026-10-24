#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <sndfile.h>
#include <stdbool.h>
#include <strings.h>  // strcasecmp()
#include <stdatomic.h>

typedef enum {
    CMD_NONE,
    CMD_PLAY,
    CMD_STOP
} player_command_t;

pthread_mutex_t command_mutex = PTHREAD_MUTEX_INITIALIZER;

player_command_t pending_command = CMD_NONE;
char pending_path[1024] = {0};

atomic_bool switching_track = false;
atomic_bool callback_active = false;
atomic_int callback_users = 0;
atomic_bool disk_eof = false;
atomic_bool playing_ended = false;
atomic_bool reader_playing = false;
atomic_bool quit = 0;
atomic_int audio_channels = 0;

#define MUSIC_DIR "/home/forrest/music"

#define SCREEN_W 800
#define SCREEN_H 600

#define PADDING 20

int update = 1;

int selection = 0;
int offset = 0;
int lines = 0;
int entries = 0;
#define MAX_ENTRIES 2048
char *entry_list[MAX_ENTRIES];

#define AUTOPLAY 1

typedef struct {
  jack_client_t *client;
  jack_port_t **ports;
  jack_ringbuffer_t *ringbuffer;
  //  SNDFILE *sndfile;
  //  SF_INFO sfinfo;
  //  int playing;
} int_client_t;

int_client_t app;


int jack_process_cb(jack_nframes_t nframes, void *arg) {

  int_client_t *a = (int_client_t *)arg;

  float *out_left = jack_port_get_buffer(a->ports[0], nframes);
  float *out_right = jack_port_get_buffer(a->ports[1], nframes);

  memset(out_left, 0, nframes * sizeof(float));
  memset(out_right, 0, nframes * sizeof(float));

  atomic_fetch_add(&callback_users, 1);
  atomic_store(&callback_active, true);

  if (atomic_load(&switching_track) ||
      !atomic_load(&reader_playing)) {
    atomic_store(&callback_active, false);
    atomic_fetch_sub(&callback_users, 1);
    return 0;
  }

  //  if (!a->playing) {
  //    atomic_store(&callback_active, false);
  //    atomic_fetch_sub(&callback_users, 1);
  //    return 0;
  // }

  static float buffer[4096 * 2];

  if (nframes > 4096) {
    atomic_store(&callback_active, false);
    atomic_fetch_sub(&callback_users, 1);
    return 0;
  }

  int channels = atomic_load(&audio_channels);

  if (channels != 1 && channels != 2) {
    atomic_store(&callback_active, false);
    atomic_fetch_sub(&callback_users, 1);
    return 0;
  }

  if (switching_track) {
    atomic_store(&callback_active, false);
    atomic_fetch_sub(&callback_users, 1);
    return 0;
  }

  size_t bytes_per_frame = channels * sizeof(float);
  size_t requested_bytes = (size_t)nframes * bytes_per_frame;

  size_t available = jack_ringbuffer_read_space(a->ringbuffer);

  size_t bytes_to_read = available < requested_bytes ? available : requested_bytes;

  bytes_to_read -= bytes_to_read % bytes_per_frame;

  if (bytes_to_read == 0) {
    if (atomic_load(&disk_eof)) {
      atomic_store(&playing_ended, true);
    }
    atomic_store(&callback_active, false);
    atomic_fetch_sub(&callback_users, 1);
    return 0;
  }
    
  jack_ringbuffer_read(
		       a->ringbuffer,
		       (char *)buffer,
		       bytes_to_read
		       );

  size_t frames = bytes_to_read / bytes_per_frame;

  for (size_t i = 0; i < frames; i++) {
    if (channels == 1) {
      // Center mono audio in both output channels.
      out_left[i] = buffer[i];
      out_right[i] = buffer[i];
    } else {
      out_left[i] = buffer[i * 2];
      out_right[i] = buffer[i * 2 + 1];
    }
  }

  // All remaining queued audio has now been consumed.
  if (atomic_load(&disk_eof) &&
      bytes_to_read == available) {
    atomic_store(&playing_ended, true);
  }

  atomic_store(&callback_active, false);
  atomic_fetch_sub(&callback_users, 1);

  return 0;
}


void *disk_reader_thread(void *arg)
{
    int_client_t *a = (int_client_t *)arg;

    SNDFILE *sndfile = NULL;
    SF_INFO sfinfo = {0};

    float disk_buffer[1024 * 2];

    while (!atomic_load(&quit)) {

        /*
         * 1. Take the pending command, if any.
         */
        player_command_t command = CMD_NONE;
        char requested_path[1024] = {0};

        pthread_mutex_lock(&command_mutex);

        if (pending_command != CMD_NONE) {
            command = pending_command;
            pending_command = CMD_NONE;

            if (command == CMD_PLAY) {
                snprintf(requested_path, sizeof(requested_path),
                         "%s", pending_path);
            }
        }

        pthread_mutex_unlock(&command_mutex);

        /*
         * 2. Handle track changes and stop requests.
         */
        if (command != CMD_NONE) {

            /*
             * Prevent new JACK callbacks from accessing the
             * ring buffer, then wait for any in-flight callback.
             */
            atomic_store(&switching_track, true);
	    atomic_store(&audio_channels, 0);
            atomic_store(&reader_playing, false);

            while (atomic_load(&callback_users) != 0) {
                usleep(100);
            }

            /*
             * No JACK callback is accessing the ring buffer now.
             * This reader thread is its only producer.
             */
            jack_ringbuffer_reset(a->ringbuffer);

            atomic_store(&disk_eof, false);
            atomic_store(&playing_ended, false);

            /*
             * This thread alone owns and closes the old file.
             */
            if (sndfile) {
                sf_close(sndfile);
                sndfile = NULL;
            }

            memset(&sfinfo, 0, sizeof(sfinfo));

            if (command == CMD_PLAY) {
                SF_INFO new_info = {0};

                SNDFILE *new_file =
                    sf_open(requested_path, SFM_READ, &new_info);

                if (!new_file) {
                    fprintf(stderr, "Cannot open %s: %s\n",
                            requested_path, sf_strerror(NULL));
                } else if (new_info.channels != 1 &&
                           new_info.channels != 2) {
                    fprintf(stderr, "Unsupported channel count: %d\n",
                            new_info.channels);
                    sf_close(new_file);
                } else {
                    sndfile = new_file;
                    sfinfo = new_info;

                    printf("Playing: %s (%d Hz, %d channel(s))\n",
                           requested_path,
                           sfinfo.samplerate,
                           sfinfo.channels);

		    atomic_store(&audio_channels, new_info.channels);
                    atomic_store(&reader_playing, true);
                }


            }

            /*
             * Publish the new state only after the file,
             * metadata and ring buffer are ready.
             */
            atomic_store(&switching_track, false);

            continue;
        }

        /*
         * 3. If no file is playing, wait for a command.
         */
        if (!sndfile || !atomic_load(&reader_playing)) {
            usleep(2000);
            continue;
        }

        /*
         * 4. EOF means stop reading, but don't mark the track
         *    complete here. JACK may still have queued audio.
         */
        if (atomic_load(&disk_eof)) {
            usleep(2000);
            continue;
        }

        const int channels = sfinfo.channels;
        const size_t bytes_per_frame =
            (size_t)channels * sizeof(float);
        const size_t requested_bytes =
            1024 * bytes_per_frame;

        /*
         * 5. Read only when enough ring-buffer space is
         *    available for a full block.
         */
        if (jack_ringbuffer_write_space(a->ringbuffer) <
            requested_bytes) {
            usleep(2000);
            continue;
        }

        sf_count_t frames =
            sf_readf_float(sndfile, disk_buffer, 1024);

        if (frames <= 0) {
            atomic_store(&disk_eof, true);
            continue;
        }

        const size_t bytes_to_write =
            (size_t)frames * bytes_per_frame;

        jack_ringbuffer_write(
            a->ringbuffer,
            (const char *)disk_buffer,
            bytes_to_write
        );
    }

    /*
     * 6. Shutdown: stop JACK from accessing the ring buffer,
     *    wait for any active callback, then close the file.
     */
    atomic_store(&switching_track, true);
    atomic_store(&reader_playing, false);
    atomic_store(&audio_channels, 0);

    while (atomic_load(&callback_users) != 0) {
        usleep(100);
    }

    if (sndfile) {
        sf_close(sndfile);
        sndfile = NULL;
    }

    return NULL;
}

void load_music_dir() {

  DIR *dir;
  struct dirent *entry;

  dir = opendir(MUSIC_DIR);
  if (dir == NULL) {
    perror("unable to open directory");
    return;
  }

  entries = 0;
  
  while ((entry = readdir(dir)) != NULL) {
    entry_list[entries] = malloc(strlen(entry->d_name) + 1);
    strcpy(entry_list[entries], entry->d_name);
    entries++;
  }

  closedir(dir);
  
}

void init_music_dir(int textHeight) {

  // find out how many lines will fit
  int y = PADDING * 2 + textHeight;
  int h = SCREEN_H - y - PADDING;
  lines = h / textHeight;

  load_music_dir();
}

void cleanup_music_dir() {
  for (int i=0; i<entries; i++) {
    free(entry_list[i]);
  }
}

void draw_music_dir(SDL_Renderer *renderer, SDL_Color textColor, TTF_Font *font, int textHeight) {

  int y = PADDING * 2 + textHeight;
  int h = SCREEN_H - y - PADDING;

  for (int i=offset; i<offset + lines && i < entries; i++) {
    
    SDL_Surface *textSurface = TTF_RenderText_Blended(font, entry_list[i], textColor);
    if (!textSurface) {
      printf("text surface error: %s\n", TTF_GetError());
      return;
    }
    SDL_Texture *textTexture = SDL_CreateTextureFromSurface(renderer, textSurface);
    int textWidth = textSurface->w;
    SDL_FreeSurface(textSurface);
    int ry = y + textHeight * (i - offset);
    SDL_Rect renderQuad = {PADDING, ry, textWidth, textHeight};

    if (i == selection) {
      SDL_SetRenderDrawColor(renderer, 0,0,0,255);
      SDL_RenderFillRect(renderer, &renderQuad);
    }

    SDL_RenderCopy(renderer, textTexture, NULL, &renderQuad);

    SDL_DestroyTexture(textTexture);
  }
}

void toggle_playback() {
    pthread_mutex_lock(&command_mutex);

    pending_command = CMD_STOP;

    pthread_mutex_unlock(&command_mutex);
}


bool play_selection() {

  // close any previously playing sound file
  //  if (app.sndfile) {
  //    app.playing = 0;
  //    sf_close(app.sndfile);
  //  } 

  if (selection < 0 || selection >= entries)
        return false;

    const char *filename = entry_list[selection];

    // Check that the filename ends in .wav (case-insensitive).
    const char *ext = strrchr(filename, '.');
    if (!ext || strcasecmp(ext, ".wav") != 0)
        return false;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s",
             MUSIC_DIR, filename);

    pthread_mutex_lock(&command_mutex);

    snprintf(pending_path, sizeof(pending_path), "%s", path);
    pending_command = CMD_PLAY;

    pthread_mutex_unlock(&command_mutex);

    return true;
}

int inc_selection() {
  
  int oldselection = selection;
  
  selection++;
  printf("down arrow pressed - incrementing selection to %d\n", selection);
  if (selection >= entries) {
    selection = entries - 1;
  }
  if (selection - offset >= lines) {
    offset = selection - lines + 1;
    if (offset < 0) offset = 0;
    printf("selection - offset >= lines(%d), new offset = %d\n", lines, offset);
  }

  return selection != oldselection;
  
}

void handle_autoplay(void)
{
    if (AUTOPLAY &&
        atomic_exchange(&playing_ended, false)) {

        while (inc_selection()) {
            if (play_selection()) {
                break;
            }
        }

        update = 1;
    }
}

int main(int argc, char* argv[]) {

  //  app.sndfile = NULL;

  jack_status_t status;
  app.client = jack_client_open("wavplayer", JackNullOption, &status);
  if (!app.client) {
    fprintf(stderr, "could not connect to jack\n");
    return 1;
  }


  app.ports = calloc(2, sizeof(jack_port_t *));

  if (!app.ports) {
    fprintf(stderr, "Could not allocate JACK ports\n");
    return 1;
  }

  for (int i = 0; i < 2; i++) {
    char port_name[32];
    snprintf(port_name, sizeof(port_name), "out_%d", i + 1);

    app.ports[i] = jack_port_register(
				      app.client,
				      port_name,
				      JACK_DEFAULT_AUDIO_TYPE,
				      JackPortIsOutput,
				      0
				      );

    if (!app.ports[i]) {
      fprintf(stderr, "Could not register JACK port %s\n", port_name);
      return 1;
    }
  }

  jack_set_process_callback(app.client, jack_process_cb, &app);

  app.ringbuffer = jack_ringbuffer_create(1024 * 1024);
  //  app.playing = 0;

  // activate jack
  if (jack_activate(app.client)) {
    fprintf(stderr, "could not activate jack client\n");
    return 1;
  }

  // Automatically connect to system playback ports
  const char **playback_ports = jack_get_ports(
					       app.client,
					       NULL,
					       JACK_DEFAULT_AUDIO_TYPE,
					       JackPortIsPhysical | JackPortIsInput);

  if (playback_ports) {
    for (int i = 0; i < 2 && playback_ports[i] && app.ports[i]; i++) {
      const char *src = jack_port_name(app.ports[i]);
      const char *dst = playback_ports[i];

      if (jack_connect(app.client, src, dst) == 0) {
	printf("Connected %s -> %s\n", src, dst);
      }
    }
    jack_free(playback_ports);
  } else {
    printf("No physical playback ports found.\n");
  }  

  // disk reader thread
  pthread_t thread_id;
  pthread_create(&thread_id, NULL, disk_reader_thread, &app);

  // init sdl libraries
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL could not initialize: %s\n", SDL_GetError());
    return 1;
  }
  if (TTF_Init() == -1) {
    printf("TTF init error: %s\n", TTF_GetError());
  }

  // create the window
  SDL_Window* window = SDL_CreateWindow("SDL2 Window Example",
					SDL_WINDOWPOS_CENTERED,
					SDL_WINDOWPOS_CENTERED,
				        SCREEN_W,
					SCREEN_H,
					SDL_WINDOW_SHOWN);
  if (window == NULL) {
    printf("window could not be created");
    SDL_Quit();
    return 1;
  }

  // create renderer
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

  // load font
  TTF_Font *font = TTF_OpenFont("ttf/heav.ttf", 24);
  if (!font) {
    printf("font load error: %s\n", TTF_GetError());
  }

  // render static text
  SDL_Color textColor = {255,255,255,255};
  SDL_Surface *textSurface = TTF_RenderText_Blended(font, "Music Player", textColor);
  if (!textSurface) {
    printf("text surface error: %s\n", TTF_GetError());
  }
  SDL_Texture *textTexture = SDL_CreateTextureFromSurface(renderer, textSurface);
  int textWidth = textSurface->w;
  int textHeight = textSurface->h;
  SDL_FreeSurface(textSurface);
  SDL_Rect renderQuad = {PADDING, PADDING, textWidth, textHeight};

  // load music dir entries
  init_music_dir(textHeight);
  
  int running = 1;
  SDL_Event event;

  while (running) {

    while (SDL_PollEvent(&event)) {

      if (event.type == SDL_QUIT) {
	running = 0;
	quit = 1;
      } else if (event.type == SDL_KEYDOWN) {
	switch (event.key.keysym.sym) {
	case SDLK_DOWN:
	  inc_selection();
	  update = 1;
	  break;
	case SDLK_UP:
	  printf("up arrow pressed\n");
	  selection--;
	  if (selection < 0) {
	    selection = 0;
	  }
	  if (selection < offset) offset = selection;
	  update = 1;
	  break;
	case SDLK_RETURN:
	  play_selection();
	  break;
	case SDLK_SPACE:
	  toggle_playback();
	  break;
	}
      }
    }

    handle_autoplay();

    if (update) {

      SDL_SetRenderDrawColor(renderer,100,100,100,255);
      SDL_RenderClear(renderer);

      SDL_RenderCopy(renderer, textTexture, NULL, &renderQuad);

      draw_music_dir(renderer, textColor, font, textHeight);

      SDL_RenderPresent(renderer);

      update = 0;
    }

    SDL_Delay(16);
  }

  SDL_DestroyTexture(textTexture);
  TTF_CloseFont(font);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  atomic_store(&quit, true);
  pthread_join(thread_id, NULL);

  jack_deactivate(app.client);
  jack_client_close(app.client);

  jack_ringbuffer_free(app.ringbuffer);
  free(app.ports);

  cleanup_music_dir();

  return 0;
}


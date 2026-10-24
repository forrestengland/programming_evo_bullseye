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

#define MUSIC_DIR "/home/forrest/music"

#define SCREEN_W 800
#define SCREEN_H 600

#define PADDING 20

int selection = 0;
int offset = 0;
int lines = 0;
int entries = 0;
#define MAX_ENTRIES 2048
char *entry_list[MAX_ENTRIES];

int quit = 0;

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

    float *out_left = jack_port_get_buffer(a->ports[0], nframes);
    float *out_right = jack_port_get_buffer(a->ports[1], nframes);

    memset(out_left, 0, nframes * sizeof(float));
    memset(out_right, 0, nframes * sizeof(float));

    if (!a->playing) return 0;

    static float buffer[4096 * 2];

    if (nframes > 4096)
        return 0;

    int channels = a->sfinfo.channels;
    if (channels != 1 && channels != 2)
        return 0;

    size_t bytes_per_frame = channels * sizeof(float);
    size_t requested_bytes = (size_t)nframes * bytes_per_frame;

    size_t available = jack_ringbuffer_read_space(a->ringbuffer);

    size_t bytes_to_read = available < requested_bytes ? available : requested_bytes;

    bytes_to_read -= bytes_to_read % bytes_per_frame;

    if (bytes_to_read == 0)
        return 0;

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

    return 0;
}

void *disk_reader_thread(void *arg) {

  int_client_t *a = (int_client_t *)arg;

  float disk_buffer[1024 * 2];

  while (!quit) {

    if (!a->playing || !a->sndfile) {
      usleep(10000);
      continue;
    }

    int channels = a->sfinfo.channels;

    if (channels != 1 && channels != 2) {
      a->playing = 0;
      continue;
    }

    size_t bytes_per_frame = channels * sizeof(float);
    size_t block_bytes = 1024 * bytes_per_frame;

    if (jack_ringbuffer_write_space(a->ringbuffer)
	>= block_bytes) {

      sf_count_t frames =
	sf_readf_float(a->sndfile, disk_buffer, 1024);

      if (frames <= 0) {
	printf("End of file reached\n");
	a->playing = 0;
	continue;
      }

      jack_ringbuffer_write(
			    a->ringbuffer,
			    (char *)disk_buffer,
			    (size_t)frames * bytes_per_frame
			    );
    } else {
      usleep(2000);
    }
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
  app.playing = !app.playing;
  printf("toggling playback to %d\n", app.playing);
}

void play_selection() {

  // close any previously playing sound file
  if (app.sndfile) {
    app.playing = 0;
    sf_close(app.sndfile);
  } 

  if (selection < 0 || selection >= entries)
    return;

  char path[1024];
  snprintf(path, sizeof(path), "%s/%s",
	   MUSIC_DIR, entry_list[selection]);

  SF_INFO info = {0};
  SNDFILE *new_file = sf_open(path, SFM_READ, &info);

  if (!new_file) {
    fprintf(stderr, "Cannot open %s: %s\n",
	    path, sf_strerror(NULL));
    return;
  }

  if (info.channels != 1 && info.channels != 2) {
    fprintf(stderr, "Unsupported channel count: %d\n",
	    info.channels);
    sf_close(new_file);
    return;
  }

  printf("Loaded: %s\n", path);
  printf("Sample rate: %d Hz, channels: %d\n",
	 info.samplerate, info.channels);

  /*
   * Note: this assumes the reader thread is not currently
   * using the previous file. Synchronize file replacement
   * before allowing track changes during playback.
   */
  //  if (app.sndfile)
  //    sf_close(app.sndfile);

  app.sfinfo = info;
  app.sndfile = new_file;

  jack_ringbuffer_reset(app.ringbuffer);
  app.playing = 1;
}

int main(int argc, char* argv[]) {

  app.sndfile = NULL;

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
  app.playing = 0;

  // activate jack
  if (jack_activate(app.client)) {
    fprintf(stderr, "could not activate jack client\n");
    return 1;
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
  int update = 1;

  while (running) {

    while (SDL_PollEvent(&event)) {

      if (event.type == SDL_QUIT) {
	running = 0;
	quit = 1;
      } else if (event.type == SDL_KEYDOWN) {
	switch (event.key.keysym.sym) {
	case SDLK_DOWN:
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

  pthread_join(thread_id, NULL);
  jack_client_close(app.client);
  jack_ringbuffer_free(app.ringbuffer);
  if (app.sndfile) sf_close(app.sndfile);
  free(app.ports);

  cleanup_music_dir();

  return 0;
}


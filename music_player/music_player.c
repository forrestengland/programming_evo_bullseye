#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

#define MUSIC_DIR "/home/forrest/music"

#define SCREEN_W 800
#define SCREEN_H 600

#define PADDING 20

int selection = 0;
int offset = 0;
int lines = 0;
int entries = 0;

void init_music_dir(int textHeight) {

  DIR *dir;
  struct dirent *entry;

  dir = opendir(MUSIC_DIR);
  if (dir == NULL) {
    perror("unable to open directory");
    return;
  }

  // find out how many lines will fit
  int y = PADDING * 2 + textHeight;
  int h = SCREEN_H - y - PADDING;
  lines = h / textHeight;

  entries = 0;
  
  while ((entry = readdir(dir)) != NULL) {
    entries++;
  }

  closedir(dir);
}

void draw_music_dir(SDL_Renderer *renderer, SDL_Color textColor, TTF_Font *font, int textHeight) {

  DIR *dir;
  struct dirent *entry;

  dir = opendir(MUSIC_DIR);
  if (dir == NULL) {
    perror("unable to open directory");
    return;
  }

  // find out how many lines will fit
  int y = PADDING * 2 + textHeight;
  int h = SCREEN_H - y - PADDING;
  //  int lines = h / textHeight;

  int i=0;
  
  while ((entry = readdir(dir)) != NULL) {

    //    printf("processing entry %d: %s\n", i, entry->d_name);

    if (i >= offset && i < offset + lines) {

      //      printf("entry is visible, rendering\n");
    
      SDL_Surface *textSurface = TTF_RenderText_Blended(font, entry->d_name, textColor);
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
    i++;
  }

  closedir(dir);  
}

// print the contents of the music directory
void print_music_dir() {

  DIR *dir;
  struct dirent *entry;

  dir = opendir(MUSIC_DIR);
  if (dir == NULL) {
    perror("unable to open directory");
    return;
  }

  printf("directory contents:\n");

  while ((entry = readdir(dir)) != NULL) {
    printf("%s\n", entry->d_name);
  }

  closedir(dir);
}

int main(int argc, char* argv[]) {

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

  init_music_dir(textHeight);
  
  int running = 1;
  SDL_Event event;
  int update = 1;

  while (running) {

    while (SDL_PollEvent(&event)) {

      if (event.type == SDL_QUIT) {
	running = 0;
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
	}
      }
    }

    if (update) {

      SDL_SetRenderDrawColor(renderer,100,100,100,255);
      SDL_RenderClear(renderer);

      SDL_RenderCopy(renderer, textTexture, NULL, &renderQuad);

      draw_music_dir(renderer, textColor, font, textHeight);

      SDL_RenderPresent(renderer);

      //      SDL_Delay(16);

      update = 0;
    }
  }

  SDL_DestroyTexture(textTexture);
  TTF_CloseFont(font);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}


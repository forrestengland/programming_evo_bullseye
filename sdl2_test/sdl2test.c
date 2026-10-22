#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>

int main(int argc, char* argv[]) {

  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL could not initialize: %s\n", SDL_GetError());
    return 1;
  }
  if (TTF_Init() == -1) {
    printf("TTF init error: %s\n", TTF_GetError());
  }

  SDL_Window* window = SDL_CreateWindow("SDL2 Window Example",
					SDL_WINDOWPOS_CENTERED,
					SDL_WINDOWPOS_CENTERED,
					800,
					600,
					SDL_WINDOW_SHOWN);
  if (window == NULL) {
    printf("window could not be created");
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

  TTF_Font* font = TTF_OpenFont("ttf/heav.ttf", 24);
  if (!font) {
    printf("font load error: %s\n", TTF_GetError());
  }

  SDL_Color textColor = {255,255,255,255};

  SDL_Surface* textSurface = TTF_RenderText_Blended(font, "Hello Compaq Evo N410c!", textColor);
  if (!textSurface) {
    printf("text surface error: %s\n", TTF_GetError());
  }

  SDL_Texture* textTexture = SDL_CreateTextureFromSurface(renderer, textSurface);
  int textWidth = textSurface->w;
  int textHeight = textSurface->h;

  SDL_FreeSurface(textSurface);

  SDL_Rect renderQuad = {(800 - textWidth) / 2, 100, textWidth, textHeight};
  
  int running = 1;
  SDL_Event event;

  while (running) {
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
	running = 0;
      }
    }
    SDL_SetRenderDrawColor(renderer,30,30,46,255);
    SDL_RenderClear(renderer);

    SDL_RenderCopy(renderer, textTexture, NULL, &renderQuad);

    SDL_RenderPresent(renderer);

    SDL_Delay(16);
  }

  SDL_DestroyTexture(textTexture);
  TTF_CloseFont(font);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}


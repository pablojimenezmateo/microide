#include <SDL3/SDL.h>
#include <stdio.h>
int main(void) {
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "vulkan");
  if (!SDL_Init(SDL_INIT_VIDEO)) { printf("init: %s\n", SDL_GetError()); return 2; }
  SDL_Window *w = SDL_CreateWindow("r", 640, 480, SDL_WINDOW_HIDDEN);
  SDL_Renderer *r = SDL_CreateRenderer(w, NULL);
  if (!r) { printf("renderer: %s\n", SDL_GetError()); return 2; }
  printf("driver=%s\n", SDL_GetRendererName(r));
  SDL_Texture *a = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, 8, 8);
  SDL_Texture *b = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, 16, 16);
  SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 480);
  SDL_SetRenderTarget(r, t);
  for (int i = 0; i < 3000; ++i) {
    SDL_FRect d = {(float)(i % 600), (float)(i % 400), 8, 8};
    if (i & 1) SDL_RenderTexture(r, a, NULL, &d); else SDL_RenderFillRect(r, &d);
  }
  SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
  printf("read=%p\n", (void*)s);
  return 0;
}

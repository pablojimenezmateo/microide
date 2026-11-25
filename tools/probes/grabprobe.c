#include <SDL3/SDL.h>
#include <stdio.h>

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) { printf("RESULT init-failed %s\n", SDL_GetError()); return 2; }
  SDL_Window* w = SDL_CreateWindow("grabprobe", 400, 300, 0);
  if (w == NULL) { printf("RESULT window-failed %s\n", SDL_GetError()); return 2; }
  SDL_SetWindowPosition(w, 100, 100);
  SDL_ShowWindow(w);
  printf("READY driver=%s\n", SDL_GetCurrentVideoDriver());
  fflush(stdout);

  int outside_motion_while_down = 0;
  int outside_motion_while_up = 0;
  int motion_total = 0;
  int button_down = 0, button_up = 0;
  const Uint64 deadline = SDL_GetTicks() + 12000;
  int held = 0;
  while (SDL_GetTicks() < deadline) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { held = 1; ++button_down; }
      if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) { held = 0; ++button_up; }
      if (e.type == SDL_EVENT_MOUSE_MOTION) {
        ++motion_total;
        const int outside = e.motion.x < 0 || e.motion.y < 0 || e.motion.x > 400 || e.motion.y > 300;
        if (held && outside) { ++outside_motion_while_down; }
        if (!held && outside) { ++outside_motion_while_up; }
      }
      if (e.type == SDL_EVENT_QUIT) { goto done; }
    }
    SDL_Delay(5);
  }
done:
  printf("RESULT motion=%d down=%d up=%d outside_while_down=%d outside_while_up=%d\n",
         motion_total, button_down, button_up, outside_motion_while_down,
         outside_motion_while_up);
  SDL_DestroyWindow(w);
  SDL_Quit();
  return 0;
}

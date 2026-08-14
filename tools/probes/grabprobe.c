// TD-2026-08-13-202 prerequisite probe: does the platform's implicit pointer
// grab deliver motion to us while a button is held and the pointer is OUTSIDE
// the window? If it does, SDL_CaptureMouse buys nothing and the entry is a
// WON'T DO on that backend.
//
// Answered for X11 on 2026-08-14: yes (0 outside-motion events with no button
// held, 3 with it held, same pointer path, one run). Kept because the answer is
// per-BACKEND: Wayland has never been measured, and this is what to rerun there.
//
//   cc tools/probes/grabprobe.c -o /tmp/grabprobe $(pkg-config --cflags --libs sdl3)
//   Xvfb :77 -screen 0 1280x800x24 &
//   DISPLAY=:77 /tmp/grabprobe &
//   # control, then treatment, along the SAME path — the control is the point:
//   export DISPLAY=:77
//   xdotool mousemove 200 200
//   for xy in "300 260" "600 500" "900 700" "1200 780"; do xdotool mousemove $xy; done
//   xdotool mousemove 200 200 mousedown 1
//   for xy in "300 260" "600 500" "900 700" "1200 780"; do xdotool mousemove $xy; done
//   xdotool mouseup 1
//
// A run whose RESULT line says down=0 proves nothing — the press never reached
// the app (no window manager, so input focus is not guaranteed); rerun it.
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
        // Window is 400x300; anything past that is outside it.
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

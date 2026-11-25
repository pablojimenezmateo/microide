#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <random>
#include "app/LostWakeRelay.h"
using namespace std::chrono;
int main(int argc, char** argv) {
  const bool use_relay = argc > 1 && std::atoi(argv[1]) != 0;
  const int rounds = argc > 2 ? std::atoi(argv[2]) : 20000;
  if (!SDL_Init(SDL_INIT_VIDEO)) { std::printf("init failed %s\n", SDL_GetError()); return 2; }
  SDL_Window* w = SDL_CreateWindow("race", 64, 64, 0);
  const Uint32 wake = SDL_RegisterEvents(1), nudge = SDL_RegisterEvents(1), quit = SDL_RegisterEvents(1);
  microide::app::LostWakeRelay relay(milliseconds(10), [&] { SDL_Event e{}; e.type = nudge; SDL_PushEvent(&e); });
  std::atomic<int> acked{0}; std::atomic<int> stalls{0};
  std::thread producer([&] {
    std::mt19937 rng(12345);
    for (int i = 1; i <= rounds; ++i) {
      const auto spin_until = steady_clock::now() + microseconds(rng() % 60);
      while (steady_clock::now() < spin_until) {}
      SDL_Event e{}; e.type = wake; SDL_PushEvent(&e);
      if (use_relay) relay.NotePush();
      const auto t0 = steady_clock::now();
      while (acked.load() < i) {
        if (steady_clock::now() - t0 > seconds(2)) { stalls++; std::printf("stall at round %d\n", i); std::fflush(stdout);
          SDL_Event k{}; k.type = nudge; SDL_PushEvent(&k); break; }
        std::this_thread::yield();
      }
      while (acked.load() < i) std::this_thread::yield();
    }
    SDL_Event q{}; q.type = quit; SDL_PushEvent(&q);
  });
  for (;;) {
    SDL_Event e;
    bool got;
    if (use_relay) relay.EnterBlockingWait();
    got = SDL_WaitEvent(&e);
    if (use_relay) relay.LeaveBlockingWait();
    if (!got) break;
    if (e.type == wake) acked.fetch_add(1);
    else if (e.type == quit) break;
  }
  producer.join();
  std::printf("relay=%d rounds=%d stalls=%d nudges=%llu\n", use_relay, rounds, stalls.load(), (unsigned long long)relay.nudges());
  SDL_DestroyWindow(w); SDL_Quit();
}

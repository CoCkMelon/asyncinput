// Use SDL2-compat and link with SDL2 to use SDL3 here
#include <SDL2/SDL.h>
#include <stdio.h>

int main() {
    SDL_Init(0); // Initialize without any subsystems first

    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return 1;
    }

    printf("SDL initialized successfully\n");

    SDL_Quit();
    return 0;
}

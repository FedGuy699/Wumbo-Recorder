#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <iostream>

int main(int argc, char* argv[]) {
    // Init SDL
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Wumbo Recorder",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        800, 600, SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "Window error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::cerr << "Renderer error: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Can't open X11 display\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    Window root = DefaultRootWindow(display);
    XRRScreenResources* screenRes = XRRGetScreenResources(display, root);
    RROutput primaryOutput = XRRGetOutputPrimary(display, root);
    XRROutputInfo* outputInfo = nullptr;
    XRRCrtcInfo* crtcInfo = nullptr;
    int x = 0, y = 0, width = 800, height = 600;

    for (int i = 0; i < screenRes->noutput; ++i) {
        if (screenRes->outputs[i] == primaryOutput) {
            outputInfo = XRRGetOutputInfo(display, screenRes, screenRes->outputs[i]);
            if (outputInfo && outputInfo->crtc) {
                crtcInfo = XRRGetCrtcInfo(display, screenRes, outputInfo->crtc);
                x = crtcInfo->x;
                y = crtcInfo->y;
                width = crtcInfo->width;
                height = crtcInfo->height;
            }
            break;
        }
    }

    // Use ARGB format which is more common for X11
    SDL_Texture* texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        width, height);
    if (!texture) {
        std::cerr << "Texture error: " << SDL_GetError() << std::endl;
        if (crtcInfo) XRRFreeCrtcInfo(crtcInfo);
        if (outputInfo) XRRFreeOutputInfo(outputInfo);
        XRRFreeScreenResources(screenRes);
        XCloseDisplay(display);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                running = false;
        }

        XImage* img = XGetImage(display, root, x, y, width, height, AllPlanes, ZPixmap);
        if (!img) {
            std::cerr << "Failed to get XImage\n";
            continue;
        }

        // Convert the image data if needed
        if (img->bits_per_pixel == 32) {
            // For 32-bit images, we can directly update the texture
            SDL_UpdateTexture(texture, nullptr, img->data, img->bytes_per_line);
        } else {
            // For other formats, we'd need conversion (not implemented here)
            std::cerr << "Unsupported image format: " << img->bits_per_pixel << " bpp\n";
        }

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        XDestroyImage(img);
        SDL_Delay(16); // ~60 FPS
    }

    // Cleanup
    if (crtcInfo) XRRFreeCrtcInfo(crtcInfo);
    if (outputInfo) XRRFreeOutputInfo(outputInfo);
    XRRFreeScreenResources(screenRes);
    XCloseDisplay(display);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
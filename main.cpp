#include <SDL.h>
#include <SDL_ttf.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <fstream>
#include <vector>  // Added this include

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// UI Constants
const int WINDOW_WIDTH = 800;
const int WINDOW_HEIGHT = 600;
const int TOOLBAR_HEIGHT = 50;
const float PREVIEW_SCALE = 0.4f;
const SDL_Color BUTTON_COLOR = {50, 50, 50, 255};
const SDL_Color BUTTON_HOVER_COLOR = {70, 70, 70, 255};
const SDL_Color BUTTON_TEXT_COLOR = {255, 255, 255, 255};

// Video Constants
const int TARGET_WIDTH = 1920;
const int TARGET_HEIGHT = 1080;
const int TARGET_FPS = 30;
const int TARGET_BITRATE = 8000000; // 8 Mbps
const char* TARGET_FORMAT = "mp4";

struct Button {
    SDL_Rect rect;
    std::string label;
    bool isHovered;
    bool isPressed;
};

struct RecordingContext {
    AVFormatContext* formatContext;
    AVCodecContext* videoCodecContext;
    AVStream* videoStream;
    SwsContext* swsContext;
    std::atomic<bool> isRecording;
    std::atomic<bool> isInitialized;
    std::string filename;
    int64_t videoFrameNumber;
    AVFrame* videoFrame;
    std::vector<uint8_t> frameBuffer;
};

bool initSDL(SDL_Window** window, SDL_Renderer** renderer) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
        return false;
    }

    if (TTF_Init() == -1) {
        std::cerr << "TTF_Init Error: " << TTF_GetError() << std::endl;
        SDL_Quit();
        return false;
    }

    *window = SDL_CreateWindow("Wumbo Recorder",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!*window) {
        std::cerr << "Window error: " << SDL_GetError() << std::endl;
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    *renderer = SDL_CreateRenderer(*window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!*renderer) {
        std::cerr << "Renderer error: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(*window);
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    return true;
}

bool finalizeRecording(RecordingContext& ctx) {
    if (!ctx.isInitialized) return false;

    // Create output context
    const AVOutputFormat* outputFormat = av_guess_format("mp4", nullptr, nullptr);
    if (!outputFormat) {
        std::cerr << "Could not find MP4 output format\n";
        return false;
    }

    if (avformat_alloc_output_context2(&ctx.formatContext, 
                                     const_cast<AVOutputFormat*>(outputFormat), 
                                     nullptr, ctx.filename.c_str()) < 0) {
        std::cerr << "Could not create output context\n";
        return false;
    }

    ctx.videoStream = avformat_new_stream(ctx.formatContext, nullptr);
    if (!ctx.videoStream) {
        std::cerr << "Could not create video stream\n";
        return false;
    }

    if (avcodec_parameters_from_context(ctx.videoStream->codecpar, ctx.videoCodecContext) < 0) {
        std::cerr << "Could not copy video codec parameters\n";
        return false;
    }

    if (!(ctx.formatContext->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ctx.formatContext->pb, ctx.filename.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Could not open output file\n";
            return false;
        }
    }

    if (avformat_write_header(ctx.formatContext, nullptr) < 0) {
        std::cerr << "Could not write header\n";
        return false;
    }

    // Process all stored frames
    size_t frameSize = av_image_get_buffer_size(ctx.videoCodecContext->pix_fmt,
                                              ctx.videoCodecContext->width,
                                              ctx.videoCodecContext->height, 1);
    size_t frameCount = ctx.frameBuffer.size() / frameSize;

    for (size_t i = 0; i < frameCount; i++) {
        const uint8_t* frameData = ctx.frameBuffer.data() + i * frameSize;
        
        // Copy frame data to AVFrame
        av_image_fill_arrays(ctx.videoFrame->data, ctx.videoFrame->linesize,
                           frameData, ctx.videoCodecContext->pix_fmt,
                           ctx.videoCodecContext->width, ctx.videoCodecContext->height, 1);
        
        ctx.videoFrame->pts = i;

        AVPacket* pkt = av_packet_alloc();
        if (avcodec_send_frame(ctx.videoCodecContext, ctx.videoFrame) < 0) {
            std::cerr << "Error sending frame to encoder\n";
            av_packet_free(&pkt);
            continue;
        }

        while (true) {
            int ret = avcodec_receive_packet(ctx.videoCodecContext, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                std::cerr << "Error receiving packet\n";
                break;
            }

            av_packet_rescale_ts(pkt, ctx.videoCodecContext->time_base, ctx.videoStream->time_base);
            pkt->stream_index = ctx.videoStream->index;
            if (av_interleaved_write_frame(ctx.formatContext, pkt) < 0) {
                std::cerr << "Error writing video packet\n";
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }

    // Flush encoder
    AVPacket* pkt = av_packet_alloc();
    avcodec_send_frame(ctx.videoCodecContext, nullptr);
    while (avcodec_receive_packet(ctx.videoCodecContext, pkt) >= 0) {
        av_packet_rescale_ts(pkt, ctx.videoCodecContext->time_base, ctx.videoStream->time_base);
        pkt->stream_index = ctx.videoStream->index;
        av_interleaved_write_frame(ctx.formatContext, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    av_write_trailer(ctx.formatContext);

    if (!(ctx.formatContext->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ctx.formatContext->pb);
    }

    return true;
}

void drawButton(SDL_Renderer* renderer, Button& button, TTF_Font* font) {
    SDL_SetRenderDrawColor(renderer, 
        button.isHovered ? BUTTON_HOVER_COLOR.r : BUTTON_COLOR.r,
        button.isHovered ? BUTTON_HOVER_COLOR.g : BUTTON_COLOR.g,
        button.isHovered ? BUTTON_HOVER_COLOR.b : BUTTON_COLOR.b,
        255);
    SDL_RenderFillRect(renderer, &button.rect);

    SDL_SetRenderDrawColor(renderer, 100, 100, 100, 255);
    SDL_RenderDrawRect(renderer, &button.rect);

    SDL_Surface* textSurface = TTF_RenderText_Solid(font, button.label.c_str(), BUTTON_TEXT_COLOR);
    if (textSurface) {
        SDL_Texture* textTexture = SDL_CreateTextureFromSurface(renderer, textSurface);
        if (textTexture) {
            int textW = textSurface->w;
            int textH = textSurface->h;
            SDL_Rect textRect = {
                button.rect.x + (button.rect.w - textW) / 2,
                button.rect.y + (button.rect.h - textH) / 2,
                textW,
                textH
            };
            SDL_RenderCopy(renderer, textTexture, NULL, &textRect);
            SDL_DestroyTexture(textTexture);
        }
        SDL_FreeSurface(textSurface);
    }
}

bool initRecording(RecordingContext& ctx, int width, int height) {
    ctx.filename = "recording.mp4";
    ctx.isInitialized = false;
    ctx.frameBuffer.clear();

    // Initialize codec and conversion context
    const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!videoCodec) {
        std::cerr << "H.264 codec not found\n";
        return false;
    }

    ctx.videoCodecContext = avcodec_alloc_context3(videoCodec);
    if (!ctx.videoCodecContext) {
        std::cerr << "Could not allocate video codec context\n";
        return false;
    }

    ctx.videoCodecContext->codec_id = AV_CODEC_ID_H264;
    ctx.videoCodecContext->bit_rate = TARGET_BITRATE;
    ctx.videoCodecContext->width = TARGET_WIDTH;
    ctx.videoCodecContext->height = TARGET_HEIGHT;
    ctx.videoCodecContext->time_base = (AVRational){1, TARGET_FPS};
    ctx.videoCodecContext->framerate = (AVRational){TARGET_FPS, 1};
    ctx.videoCodecContext->gop_size = 10;
    ctx.videoCodecContext->max_b_frames = 1;
    ctx.videoCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    
    av_opt_set(ctx.videoCodecContext->priv_data, "preset", "medium", 0);
    av_opt_set(ctx.videoCodecContext->priv_data, "tune", "film", 0);
    av_opt_set(ctx.videoCodecContext->priv_data, "crf", "18", 0);

    if (avcodec_open2(ctx.videoCodecContext, videoCodec, nullptr) < 0) {
        std::cerr << "Could not open video codec\n";
        return false;
    }

    ctx.swsContext = sws_getContext(width, height, AV_PIX_FMT_BGR24,
                                   TARGET_WIDTH, TARGET_HEIGHT, AV_PIX_FMT_YUV420P,
                                   SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!ctx.swsContext) {
        std::cerr << "Could not create SWS context\n";
        return false;
    }

    ctx.videoFrame = av_frame_alloc();
    ctx.videoFrame->format = ctx.videoCodecContext->pix_fmt;
    ctx.videoFrame->width = ctx.videoCodecContext->width;
    ctx.videoFrame->height = ctx.videoCodecContext->height;
    if (av_frame_get_buffer(ctx.videoFrame, 32) < 0) {
        std::cerr << "Could not allocate video frame data\n";
        return false;
    }

    ctx.videoFrameNumber = 0;
    ctx.isInitialized = true;
    return true;
}

void writeFrame(RecordingContext& ctx, const uint8_t* data, int width, int height) {
    if (!ctx.isRecording || !ctx.isInitialized) return;

    // Convert frame to YUV420P
    const uint8_t* srcData[1] = { data };
    int srcLinesize[1] = { width * 3 };
    
    sws_scale(ctx.swsContext, srcData, srcLinesize, 0, height, 
             ctx.videoFrame->data, ctx.videoFrame->linesize);

    ctx.videoFrame->pts = ctx.videoFrameNumber++;

    // Store the frame data in memory
    size_t frameSize = av_image_get_buffer_size(ctx.videoCodecContext->pix_fmt, 
                                              ctx.videoCodecContext->width, 
                                              ctx.videoCodecContext->height, 1);
    std::vector<uint8_t> frameData(frameSize);
    av_image_copy_to_buffer(frameData.data(), frameSize,
                          (const uint8_t* const*)ctx.videoFrame->data,
                          ctx.videoFrame->linesize,
                          ctx.videoCodecContext->pix_fmt,
                          ctx.videoCodecContext->width,
                          ctx.videoCodecContext->height, 1);
    
    ctx.frameBuffer.insert(ctx.frameBuffer.end(), frameData.begin(), frameData.end());
}

void cleanupRecording(RecordingContext& ctx) {
    if (!ctx.isInitialized) return;

    // First finalize the recording
    finalizeRecording(ctx);

    // Then clean up resources
    if (ctx.videoFrame) av_frame_free(&ctx.videoFrame);
    if (ctx.swsContext) sws_freeContext(ctx.swsContext);
    if (ctx.videoCodecContext) avcodec_free_context(&ctx.videoCodecContext);
    if (ctx.formatContext) avformat_free_context(ctx.formatContext);
    
    ctx.isRecording = false;
    ctx.isInitialized = false;
    ctx.frameBuffer.clear();
}

int main(int argc, char* argv[]) {
    av_log_set_level(AV_LOG_ERROR);
    
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!initSDL(&window, &renderer)) {
        return 1;
    }

    TTF_Font* font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16);
    if (!font) {
        std::cerr << "Failed to load font: " << TTF_GetError() << std::endl;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Can't open X11 display\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_CloseFont(font);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    Window root = DefaultRootWindow(display);
    XRRScreenResources* screenRes = XRRGetScreenResources(display, root);
    RROutput primaryOutput = XRRGetOutputPrimary(display, root);
    XRROutputInfo* outputInfo = nullptr;
    XRRCrtcInfo* crtcInfo = nullptr;
    int x = 0, y = 0, width = 0, height = 0;

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

    if (width == 0 || height == 0) {
        std::cerr << "Could not get screen resolution, defaulting to 1920x1080\n";
        width = 1920;
        height = 1080;
    }

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
        TTF_CloseFont(font);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    Button recordButton = {
        {WINDOW_WIDTH - 150, TOOLBAR_HEIGHT - 40, 120, 30},
        "Start Recording",
        false,
        false
    };

    RecordingContext recordingContext{};
    recordingContext.isRecording = false;
    recordingContext.isInitialized = false;
    bool running = true;
    SDL_Event event;

    // Timing control for consistent frame rate
    auto lastFrameTime = std::chrono::steady_clock::now();
    const std::chrono::milliseconds frameDuration(1000 / TARGET_FPS);

    while (running) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = now - lastFrameTime;
        
        // Process events
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_MOUSEMOTION) {
                int mouseX = event.motion.x;
                int mouseY = event.motion.y;
                recordButton.isHovered = 
                    (mouseX >= recordButton.rect.x && 
                     mouseX <= recordButton.rect.x + recordButton.rect.w &&
                     mouseY >= recordButton.rect.y && 
                     mouseY <= recordButton.rect.y + recordButton.rect.h);
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    int mouseX = event.button.x;
                    int mouseY = event.button.y;
                    if (mouseX >= recordButton.rect.x && 
                        mouseX <= recordButton.rect.x + recordButton.rect.w &&
                        mouseY >= recordButton.rect.y && 
                        mouseY <= recordButton.rect.y + recordButton.rect.h) {
                        recordButton.isPressed = true;
                    }
                }
            }
            else if (event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_LEFT && recordButton.isPressed) {
                    recordButton.isPressed = false;
                    recordingContext.isRecording = !recordingContext.isRecording;
                    recordButton.label = recordingContext.isRecording ? "Stop Recording" : "Start Recording";
                    
                    if (recordingContext.isRecording) {
                        if (!initRecording(recordingContext, width, height)) {
                            recordingContext.isRecording = false;
                            recordButton.label = "Start Recording";
                            std::cerr << "Failed to start recording\n";
                        }
                    } else {
                        cleanupRecording(recordingContext);
                    }
                }
            }
        }

        // Capture screen
        XImage* img = XGetImage(display, root, x, y, width, height, AllPlanes, ZPixmap);
        if (img) {
            if (img->bits_per_pixel == 32) {
                // For 32bpp, we need to determine if it's ARGB or BGRA
                bool isARGB = (img->red_mask == 0xff0000 && 
                            img->green_mask == 0xff00 && 
                            img->blue_mask == 0xff);
                
                bool isBGRA = (img->blue_mask == 0xff0000 && 
                            img->green_mask == 0xff00 && 
                            img->red_mask == 0xff);
                
                if (isARGB || isBGRA) {
                    // Update texture with correct format
                    SDL_UpdateTexture(texture, nullptr, img->data, img->bytes_per_line);
                    
                    if (recordingContext.isRecording) {
                        // Convert to BGR24 for FFmpeg
                        uint8_t* bgrData = new uint8_t[width * height * 3];
                        for (int y = 0; y < height; y++) {
                            for (int x = 0; x < width; x++) {
                                uint32_t pixel = *((uint32_t*)(img->data + y * img->bytes_per_line + x * 4));
                                uint8_t r = (pixel & img->red_mask) >> 16;
                                uint8_t g = (pixel & img->green_mask) >> 8;
                                uint8_t b = (pixel & img->blue_mask);
                                
                                int offset = (y * width + x) * 3;
                                bgrData[offset] = b;
                                bgrData[offset + 1] = g;
                                bgrData[offset + 2] = r;
                            }
                        }
                        writeFrame(recordingContext, bgrData, width, height);
                        delete[] bgrData;
                    }
                } else {
                    std::cerr << "Unsupported 32bpp pixel format\n";
                }
            } else if (img->bits_per_pixel == 24) {
                // Directly use 24bpp data
                SDL_UpdateTexture(texture, nullptr, img->data, img->bytes_per_line);
                if (recordingContext.isRecording) {
                    writeFrame(recordingContext, (uint8_t*)img->data, width, height);
                }
            } else {
                std::cerr << "Unsupported image format: " << img->bits_per_pixel << " bpp\n";
            }
            XDestroyImage(img);
        }

        // Render UI
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255);
        SDL_Rect toolbarRect = {0, 0, WINDOW_WIDTH, TOOLBAR_HEIGHT};
        SDL_RenderFillRect(renderer, &toolbarRect);

        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_Rect previewBgRect = {0, TOOLBAR_HEIGHT, WINDOW_WIDTH, WINDOW_HEIGHT - TOOLBAR_HEIGHT};
        SDL_RenderFillRect(renderer, &previewBgRect);

        int previewWidth = static_cast<int>(width * PREVIEW_SCALE);
        int previewHeight = static_cast<int>(height * PREVIEW_SCALE);
        int previewX = (WINDOW_WIDTH - previewWidth) / 2;
        int previewY = TOOLBAR_HEIGHT + (WINDOW_HEIGHT - TOOLBAR_HEIGHT - previewHeight) / 2;

        SDL_Rect previewRect = {previewX, previewY, previewWidth, previewHeight};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &previewRect);
        SDL_SetRenderDrawColor(renderer, 60, 60, 60, 255);
        SDL_RenderDrawRect(renderer, &previewRect);

        SDL_RenderCopy(renderer, texture, nullptr, &previewRect);
        drawButton(renderer, recordButton, font);

        if (recordingContext.isRecording) {
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
            SDL_Rect recordingIndicator = {10, 10, 10, 10};
            SDL_RenderFillRect(renderer, &recordingIndicator);
        }

        SDL_RenderPresent(renderer);

        // Frame rate control
        auto processingTime = std::chrono::steady_clock::now() - now;
        if (processingTime < frameDuration) {
            std::this_thread::sleep_for(frameDuration - processingTime);
        }
        lastFrameTime = std::chrono::steady_clock::now();
    }

    if (recordingContext.isRecording) {
        cleanupRecording(recordingContext);
    }

    if (crtcInfo) XRRFreeCrtcInfo(crtcInfo);
    if (outputInfo) XRRFreeOutputInfo(outputInfo);
    XRRFreeScreenResources(screenRes);
    XCloseDisplay(display);
    TTF_CloseFont(font);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
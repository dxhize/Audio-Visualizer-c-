#define MA_NO_WAV
#define MA_NO_FLAC
#define MINIAUDIO_IMPLEMENTATION

#include "miniaudio.h"
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "kiss_fft.h"

#define SAMPLE_WINDOW 512
#define MAX_BARS 256
#define COLOR_COUNT 7

static float *g_visualiser_buffer = NULL;
static int g_sample_window = SAMPLE_WINDOW;

static void dataCallback(ma_device *pDevice, void *pOutput,
                         const void *pInput, ma_uint32 frameCount);

static int clamp_int(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static void initialize_colors(void)
{
    if (!has_colors()) return;

    start_color();
    use_default_colors();

    /* Bright foregrounds against the terminal's default background. */
    init_pair(1, COLOR_CYAN,    -1);
    init_pair(2, COLOR_GREEN,   -1);
    init_pair(3, COLOR_YELLOW,  -1);
    init_pair(4, COLOR_MAGENTA, -1);
    init_pair(5, COLOR_BLUE,    -1);
    init_pair(6, COLOR_RED,     -1);
    init_pair(7, COLOR_WHITE,   -1);
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <audio-file>\n", argv[0]);
        return 1;
    }

    ma_decoder decoder;
    ma_device device;
    ma_device_config device_config;
    ma_result result;
    int decoder_initialized = 0;
    int device_initialized = 0;
    int curses_initialized = 0;
    int exit_code = 1;

    float *smoothed_heights = NULL;
    int *bar_colors = NULL;
    kiss_fft_cfg fft_cfg = NULL;
    kiss_fft_cpx *fft_in = NULL;
    kiss_fft_cpx *fft_out = NULL;

    result = ma_decoder_init_file(argv[1], NULL, &decoder);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Could not open '%s' (decoder error: %d).\n", argv[1], result);
        goto cleanup;
    }
    decoder_initialized = 1;

    g_visualiser_buffer = (float *)calloc((size_t)g_sample_window, sizeof(float));
    smoothed_heights = (float *)calloc(MAX_BARS, sizeof(float));
    bar_colors = (int *)malloc(MAX_BARS * sizeof(int));
    fft_in = (kiss_fft_cpx *)malloc((size_t)g_sample_window * sizeof(*fft_in));
    fft_out = (kiss_fft_cpx *)malloc((size_t)g_sample_window * sizeof(*fft_out));
    fft_cfg = kiss_fft_alloc(g_sample_window, 0, NULL, NULL);

    if (!g_visualiser_buffer || !smoothed_heights || !bar_colors ||
        !fft_in || !fft_out || !fft_cfg) {
        fprintf(stderr, "Failed to allocate visualizer memory.\n");
        goto cleanup;
    }

    srand((unsigned int)time(NULL));
    for (int i = 0; i < MAX_BARS; ++i)
        bar_colors[i] = 1 + rand() % COLOR_COUNT;
        
    device_config = ma_device_config_init(ma_device_type_playback);
    device_config.playback.format = decoder.outputFormat;
    device_config.playback.channels = decoder.outputChannels;
    device_config.sampleRate = decoder.outputSampleRate;
    device_config.dataCallback = dataCallback;
    device_config.pUserData = &decoder;

    result = ma_device_init(NULL, &device_config, &device);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Failed to initialize playback device (error: %d).\n", result);
        goto cleanup;
    }
    device_initialized = 1;

    initscr();
    curses_initialized = 1;
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    initialize_colors();

    srand((unsigned int)time(NULL));
    if (ma_device_start(&device) != MA_SUCCESS) {
        endwin();
        curses_initialized = 0;
        fprintf(stderr, "Failed to start audio playback.\n");
        goto cleanup;
    }

    while (ma_device_is_started(&device)) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == 27) break;

        int screen_height, screen_width;
        getmaxyx(stdscr, screen_height, screen_width);

        if (screen_height < 3 || screen_width < 4) {
            erase();
            mvaddstr(0, 0, "Enlarge terminal to view visualizer | q: quit");
            refresh();
            ma_sleep(8);
            continue;
        }

        /* Snapshot the most recent sample window for the FFT. */
        for (int i = 0; i < g_sample_window; ++i) {
            fft_in[i].r = g_visualiser_buffer[i];
            fft_in[i].i = 0.0f;
        }
        kiss_fft(fft_cfg, fft_in, fft_out);

        /* Two columns per bar: one hash plus one column of breathing room. */
        int bar_count = clamp_int(screen_width / 2, 1, MAX_BARS);
        int usable_height = screen_height - 1;
        int bins_per_bar = (g_sample_window / 2) / bar_count;
        if (bins_per_bar < 1) bins_per_bar = 1;

        erase();

        for (int bar = 0; bar < bar_count; ++bar) {
            int first_bin = 1 + bar * bins_per_bar;
            int last_bin = first_bin + bins_per_bar;
            if (last_bin > g_sample_window / 2) last_bin = g_sample_window / 2;

            float magnitude_sum = 0.0f;
            int bin_count = 0;
            for (int bin = first_bin; bin < last_bin; ++bin) {
                float real = fft_out[bin].r;
                float imag = fft_out[bin].i;
                magnitude_sum += sqrtf(real * real + imag * imag);
                ++bin_count;
            }

            float average_magnitude = bin_count ? magnitude_sum / (float)bin_count : 0.0f;
            /* Log-like scaling keeps quieter frequencies visible without huge peaks. */
            float target_height = log1pf(average_magnitude * 7.0f) *
                                  ((float)usable_height / 5.0f);
            if (target_height > (float)usable_height)
                target_height = (float)usable_height;

            /* Quick attack, smoother release. */
            if (target_height > smoothed_heights[bar]) {
                smoothed_heights[bar] = target_height * 0.72f + smoothed_heights[bar] * 0.28f;
            } else {
                smoothed_heights[bar] = smoothed_heights[bar] * 0.82f + target_height * 0.18f;
            }

            int height = clamp_int((int)smoothed_heights[bar]-2, 0, usable_height);
            int x = bar * 2;
            int color_pair = has_colors() ? bar_colors[bar] : 0;

            for (int row = 0; row < height; ++row) {
                int y = screen_height - 1 - row;

                if (has_colors()) {
                    // Generate a completely new color for EVERY hash.
                    int color_pair = 1 + rand() % COLOR_COUNT;

                    // Randomly choose the character too.
                    const char symbols[] = "$";
                    char symbol = symbols[rand() % (sizeof(symbols) - 1)];

                    attron(COLOR_PAIR(color_pair) | A_BOLD);
                    mvaddch(y, x, symbol);
                    attroff(COLOR_PAIR(color_pair) | A_BOLD);
                } else {
                    mvaddch(y, x, '#');
                }
            }

            if (color_pair) attroff(COLOR_PAIR(color_pair) | A_BOLD);
        }

        /* Small unobtrusive footer, kept clear of the spectrum. */
        if (screen_height >= 4 && screen_width >= 30) {
            attron(A_DIM);
            mvaddnstr(0, 1, "♪  AUDIO SPECTRUM   |   Q: QUIT", screen_width - 2);
            attroff(A_DIM);
        }

        refresh();
        ma_sleep(8);
    }

    exit_code = 0;

cleanup:
    if (curses_initialized) endwin();
    if (device_initialized) {
        ma_device_stop(&device);
        ma_device_uninit(&device);
    }
    if (decoder_initialized) ma_decoder_uninit(&decoder);

    free(g_visualiser_buffer);
    free(smoothed_heights);
    free(bar_colors);
    free(fft_in);
    free(fft_out);
    if (fft_cfg) kiss_fft_free(fft_cfg);

    if (exit_code == 0)
        printf("Audio engine shut down cleanly. Goodbye!\n");
    return exit_code;
}

static void dataCallback(ma_device *pDevice, void *pOutput,
                         const void *pInput, ma_uint32 frameCount)
{
    ma_decoder *decoder = (ma_decoder *)pDevice->pUserData;
    if (!decoder || !g_visualiser_buffer || !pOutput) return;

    ma_uint64 frames_read = 0;
    ma_decoder_read_pcm_frames(decoder, pOutput, frameCount, &frames_read);

    const float *output = (const float *)pOutput;
    ma_uint32 channels = pDevice->playback.channels;
    if (channels == 0) return;

    /* Keep the latest samples; account for mono, stereo, or multichannel audio. */
    ma_uint32 samples_to_copy = frames_read < (ma_uint64)g_sample_window
                              ? (ma_uint32)frames_read : (ma_uint32)g_sample_window;
    for (ma_uint32 i = 0; i < samples_to_copy; ++i) {
        float sum = 0.0f;
        for (ma_uint32 channel = 0; channel < channels; ++channel)
            sum += output[(size_t)i * channels + channel];
        g_visualiser_buffer[i] = sum / (float)channels;
    }
    for (ma_uint32 i = samples_to_copy; i < (ma_uint32)g_sample_window; ++i)
        g_visualiser_buffer[i] = 0.0f;

    (void)pInput;
}

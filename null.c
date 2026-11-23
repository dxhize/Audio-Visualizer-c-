#define MA_NO_WAV
#define MA_NO_FLAC
#define MINIAUDIO_IMPLEMENTATION 

#include "miniaudio.h"
#include <ncurses.h>
#include <stdio.h>
#include "kiss_fft.h"
#include <math.h>

void dataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

float *g_visualiser_buffer = NULL;
int g_sample_window = 512;

int main(int argc, char *argv[])
{
    // Ensure the user provided a file argument in the terminal
    if (argc != 2)
    {
        printf("Usage: ./appname <file.mp3>\n");
        return 1;
    }

    // Declare the core Miniaudio structures needed for low-level device playback
    ma_result decoder_result;
    ma_result device_result;
    ma_decoder decoder;
    ma_device device;
    ma_device_config device_config;

    /* 
     * DATA STREAM PHASE 1: Open the file on disk.
     */
    decoder_result = ma_decoder_init_file(argv[1], NULL, &decoder);
    
    // Error resolution block for handling file and decoder issues
    if (decoder_result != MA_SUCCESS) 
    {
        if (decoder_result == MA_INVALID_FILE)
        {
            printf("File path incorrect / doesn't exist!\n");
            return 1;
        }
        else if (decoder_result == MA_FORMAT_NOT_SUPPORTED)
        {
            printf("Unsupported format. Use (.mp3)!\n");
            return 1;
        }
        else {
            printf("Unknown error loading file!\n");
            return 1;
        }
    }

    // Success validation: Print out the decoded properties of the audio stream
    printf("Opened %s successfully!\n", argv[1]);
    printf("Channels: %u\n", decoder.outputChannels);
    printf("Sample Rate: %u Hz\n", decoder.outputSampleRate);
    printf("Attempting to communicate with output device...\n");

    // PERSISTENT MEMORY ALLOCATIONS (HEAP)
    // Allocating buffers completely BEFORE starting the hardware stream threads
    g_visualiser_buffer = (float*)malloc(g_sample_window * sizeof(float));
    if (g_visualiser_buffer == NULL)
    {
        printf("Failed to allocate visualizer memory buffer!\n");
        ma_decoder_uninit(&decoder);
        return 1;
    }
    // Initialize global buffer data safely to zero
    for(int i = 0; i < g_sample_window; i++) g_visualiser_buffer[i] = 0.0f;

    // Allocate persistent visual bar tracking layouts
    int total_bars = 60; 
    int bins_per_bar = g_sample_window / total_bars; 
    
    float* g_smoothed_heights = (float*)malloc(total_bars * sizeof(float));
    if (g_smoothed_heights == NULL) {
        printf("Failed to allocate smoothing array!\n");
        free(g_visualiser_buffer);
        ma_decoder_uninit(&decoder);
        return 1;
    }
    for(int i = 0; i < total_bars; i++) g_smoothed_heights[i] = 0.0f;

    // Allocate specialized FFT structures
    kiss_fft_cfg cfg = kiss_fft_alloc(g_sample_window, 0, NULL, NULL);
    kiss_fft_cpx* fft_in = (kiss_fft_cpx*)malloc(g_sample_window * sizeof(kiss_fft_cpx));
    kiss_fft_cpx* fft_out = (kiss_fft_cpx*)malloc(g_sample_window * sizeof(kiss_fft_cpx));

    /* 
     * DATA STREAM PHASE 2: Configure the hardware playback device.
     */
    device_config = ma_device_config_init(ma_device_type_playback);
    device_config.playback.format   = decoder.outputFormat;
    device_config.playback.channels = decoder.outputChannels;
    device_config.sampleRate        = decoder.outputSampleRate;
    
    /* 
     * DATA STREAM PHASE 3: Link the background thread pipeline.
     */
    device_config.dataCallback      = dataCallback; 
    device_config.pUserData         = &decoder;

    /*
     * DATA STREAM PHASE 4: Connect to the sound card.
     */
    device_result = ma_device_init(NULL, &device_config, &device);
    if (device_result != MA_SUCCESS)
    {
        printf("Failed to initialize the playback hardware device!\n");
        free(g_smoothed_heights);
        free(g_visualiser_buffer);
        free(cfg); free(fft_in); free(fft_out);
        ma_decoder_uninit(&decoder);
        return 1;
    }

    /*
     * DATA STREAM PHASE 5: Start playback threads.
     */
    device_result = ma_device_start(&device);
    if (device_result != MA_SUCCESS)
    {
        printf("Failed to start the audio playback thread!\n");
        free(g_smoothed_heights);
        free(g_visualiser_buffer);
        free(cfg); free(fft_in); free(fft_out);
        ma_device_uninit(&device);
        ma_decoder_uninit(&decoder);
        return 1;
    }

    // Initialize the Ncurses visualization window
    initscr();             
    noecho();              
    curs_set(0);           
    nodelay(stdscr, TRUE); 

    // MAIN VISUALIZER ENGINE LOOP
    while (ma_device_is_started(&device))
    {
        // Check for user keyboard escape signals (Press 'q' or 'Ctrl+C' to exit gracefully)
        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            break;
        }

        // 1. Copy the latest wave data safely into the FFT input layout
        for (int i = 0; i < g_sample_window; i++)
        {
            fft_in[i].r = g_visualiser_buffer[i];
            fft_in[i].i = 0.0f;
        }

        // 2. Execute the digital signal processing transform
        kiss_fft(cfg, fft_in, fft_out);

        erase(); // Clear screen buffer

        int screen_height, screen_width;
        getmaxyx(stdscr, screen_height, screen_width);

        // 3. Process and Draw Frequencies columns
        for (int bar = 0; bar < total_bars; bar++)
        {
            float bar_sum = 0.0f;
            
            for (int b = 0; b < bins_per_bar; b++)
            {
                int bin_index = (bar * bins_per_bar) + b;
                if (bin_index >= g_sample_window) break;

                float mag = sqrtf((fft_out[bin_index].r * fft_out[bin_index].r) + 
                                  (fft_out[bin_index].i * fft_out[bin_index].i));
                bar_sum += mag;
            }

            // Continuous Floating-point dampening computation
            float target_height = bar_sum * 0.25f;

            if (target_height > g_smoothed_heights[bar]) {
                g_smoothed_heights[bar] = target_height; // Snap up instantly on loud beats
            } else {
                g_smoothed_heights[bar] = (g_smoothed_heights[bar] * 0.85f) + (target_height * 0.15f); // Glide down
            }   

            int final_draw_height = (int)g_smoothed_heights[bar]; 
            if (final_draw_height >= screen_height) final_draw_height = screen_height - 1;

            // Draw the smoothed vertical bars columns
            for (int row = 0; row < final_draw_height; row++)
            {
                mvaddch(screen_height - 1 - row, bar * 2, '#'); 
            }
        }

        refresh();   // Swap window matrix to view display
        ma_sleep(16); // Standard 60 FPS loop delay tracking
    }

    /*
     * TEARDOWN DEALLOCATIONS BLOCK
     */
    endwin(); 
    ma_device_uninit(&device);
    ma_decoder_uninit(&decoder);
    
    // Release allocations cleanly
    free(g_smoothed_heights);
    free(g_visualiser_buffer);
    free(cfg);
    free(fft_in);
    free(fft_out);
    
    printf("Audio engine shut down cleanly. Goodbye!\n");
    return 0;
}

void dataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    ma_decoder* pDecoder = (ma_decoder*)pDevice->pUserData;
    if (pDecoder == NULL || g_visualiser_buffer == NULL) {
        return;
    }

    ma_uint64 framesRead = 0;
    ma_decoder_read_pcm_frames(pDecoder, pOutput, frameCount, &framesRead);
    
    float *floatOutput = (float*)pOutput;
    
    // Bounds check protection against Pipewire/ALSA sample deviations
    for (int i = 0; i < g_sample_window && i < frameCount; i++)
    {
        float avg = (floatOutput[i * 2] + floatOutput[(i * 2) + 1]) / 2.0f; 
        g_visualiser_buffer[i] = avg;
    }

    (void)pInput;
}

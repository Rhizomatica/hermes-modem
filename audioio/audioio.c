/* Audio subsystem
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */


#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <ffaudio/audio.h>
#include "std.h"
#ifdef FF_LINUX
#include <time.h>
#endif

#include "ring_buffer_posix.h"
#include "shm_posix.h"
#include "../datalink/defines.h"
#include "os_interop.h"

#include "audioio.h"

// bool shutdown_;
extern bool shutdown_;

cbuf_handle_t capture_buffer;
cbuf_handle_t playback_buffer;

int audio_subsystem;

struct conf {
    const char *cmd;
    ffaudio_conf buf;
    uint8_t flags;
    uint8_t exclusive;
    uint8_t hwdev;
    uint8_t loopback;
    uint8_t nonblock;
    uint8_t wav;
};


static inline void ffthread_sleep(ffuint msec)
{
#ifdef FF_WIN
    Sleep(msec);
#else
    struct timespec ts = {
        .tv_sec = msec / 1000,
        .tv_nsec = (msec % 1000) * 1000000,
    };
    nanosleep(&ts, NULL);
#endif
}


void *radio_playback_thread(void *device_ptr)
{
    ffaudio_interface *audio;
    struct conf conf = {};
    conf.buf.app_name = "mercury_playback";
    conf.buf.format = FFAUDIO_F_INT32;
    conf.buf.sample_rate = 48000;
    conf.buf.channels = 2;
    conf.buf.device_id = (const char *) device_ptr;
    uint32_t period_ms;
    uint32_t period_bytes;


#if defined(_WIN32)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    conf.buf.buffer_length_msec = 30;
    period_ms = conf.buf.buffer_length_msec / 3;
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#endif

    period_bytes = conf.buf.sample_rate * sizeof(double) * period_ms / 1000;

    //printf("period_ms: %u\n", period_ms);
    //printf("period_size: %u\n", period_bytes);
    conf.flags = FFAUDIO_PLAYBACK;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury_playback";

    int r;
    ffaudio_buf *b;
    ffaudio_conf *cfg;

    ffuint frame_size;
    ffuint msec_bytes;

    // input is int16_t (aka. short)
    uint16_t *input_buffer = (uint16_t *) malloc(SIGNAL_BUFFER_SIZE * sizeof(int16_t));

    // output is int32_t
    int32_t *buffer_output_stereo = (int32_t *) malloc(SIGNAL_BUFFER_SIZE * sizeof(int32_t) * 2); // a big enough buffer

    ffuint total_written = 0;
    int ch_layout = STEREO;

    if ( audio->init(&aconf) != 0)
    {
        printf("Error in audio->init()\n");
        goto finish_play;
    }

    // playback code...
    b = audio->alloc();
    if (b == NULL)
    {
        printf("Error in audio->alloc()\n");
        goto finish_play;
    }

    cfg = &conf.buf;
    r = audio->open(b, cfg, conf.flags);
    if (r == FFAUDIO_EFORMAT)
        r = audio->open(b, cfg, conf.flags);
    if (r != 0)
    {
        printf("error in audio->open(): %d: %s\n", r, audio->error(b));
        goto cleanup_play;
    }

    printf("I/O playback (%s) %d bits per sample / %dHz / %dch / %dms buffer\n", conf.buf.device_id ? conf.buf.device_id : "default", cfg->format, cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);


    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    msec_bytes = cfg->sample_rate * frame_size / 1000;

#if 0 // TODO: parametrize this
    if (radio_type == RADIO_SBITX)
        ch_layout = RIGHT;
    if (radio_type == RADIO_STOCKHF)
        ch_layout = STEREO;
#endif
    ch_layout = STEREO;
    
    while (!shutdown_)
    {
        ffssize n;
        size_t buffer_size = size_buffer(playback_buffer);
        if (buffer_size >= period_bytes)
        {
            read_buffer(playback_buffer, (uint8_t *) input_buffer, period_bytes);
            n = period_bytes;
        }
        else
        {
            // we just play zeros if there is nothing to play
            memset(input_buffer, 0, period_bytes);
            if (buffer_size > 0)
                read_buffer(playback_buffer, (uint8_t *) input_buffer, buffer_size);
            n = buffer_size;
        }

        total_written = 0;

        int samples_read = n / sizeof(int16_t);

        // convert from int16 to int32
        for (int i = 0; i < samples_read; i++)
        {
            int idx = i * cfg->channels;
            if (ch_layout == LEFT)
            {
                buffer_output_stereo[idx] = (int32_t) input_buffer[i] << 16;
                buffer_output_stereo[idx + 1] = 0;
            }

            if (ch_layout == RIGHT)
            {
                buffer_output_stereo[idx] = 0;
                buffer_output_stereo[idx + 1] = (int32_t) input_buffer[i] << 16;
            }


            if (ch_layout == STEREO)
            {
                buffer_output_stereo[idx] = (int32_t) input_buffer[i] << 16;
                buffer_output_stereo[idx + 1] = buffer_output_stereo[idx];
            }
        }

        n = samples_read * frame_size;

        while (n >= frame_size)
        {
            r = audio->write(b, ((uint8_t *)buffer_output_stereo) + total_written, n);

            if (r == -FFAUDIO_ESYNC) {
                printf("detected underrun");
                continue;
            }
            if (r < 0)
            {
                printf("ffaudio.write: %s", audio->error(b));
            }
#if 0 // print time measurement
            else
            {
                printf(" %dms\n", r / msec_bytes);
            }
#endif
            total_written += r;
            n -= r;
        }
        // printf("n = %lld total written = %u\n", n, total_written);
    }

    r = audio->drain(b);
    if (r < 0)
        printf("ffaudio.drain: %s", audio->error(b));

    r = audio->stop(b);
    if (r != 0)
        printf("ffaudio.stop: %s", audio->error(b));

    r = audio->clear(b);
    if (r != 0)
        printf("ffaudio.clear: %s", audio->error(b));

cleanup_play:

    audio->free(b);

    audio->uninit();

finish_play:

    free(input_buffer);
    free(buffer_output_stereo);

    printf("radio_playback_thread exit\n");

    shutdown_ = true;

    return NULL;
}


void *radio_capture_thread(void *device_ptr)
{
    ffaudio_interface *audio;
    struct conf conf = {};
    conf.buf.app_name = "mercury_capture";
    conf.buf.format = FFAUDIO_F_INT32;
    conf.buf.sample_rate = 48000;
    conf.buf.channels = 2;
    conf.buf.device_id = (const char *) device_ptr;

#if defined(_WIN32)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    conf.buf.buffer_length_msec = 30;
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#endif

    conf.flags = FFAUDIO_CAPTURE;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury_capture";

    int r;
    ffaudio_buf *b;
    ffaudio_conf *cfg;

    ffuint frame_size;
    ffuint msec_bytes;

    int32_t *buffer = NULL;

    int ch_layout = STEREO;

    int16_t *buffer_output = NULL;

    if ( audio->init(&aconf) != 0)
    {
        printf("Error in audio->init()\n");
        goto finish_cap;
    }

    // capture code
    b = audio->alloc();
    if (b == NULL)
    {
        printf("Error in audio->alloc()\n");
        goto finish_cap;
    }

    cfg = &conf.buf;
    r = audio->open(b, cfg, conf.flags);
    if (r == FFAUDIO_EFORMAT)
        r = audio->open(b, cfg, conf.flags);
    if (r != 0)
    {
        printf("error in audio->open(): %d: %s\n", r, audio->error(b));
        goto cleanup_cap;
    }

    printf("I/O capture (%s) %d bits per sample / %dHz / %dch / %dms buffer\n", conf.buf.device_id ? conf.buf.device_id : "default", cfg->format, cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);

    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    msec_bytes = cfg->sample_rate * frame_size / 1000;

    buffer_output = (int16_t *) malloc(SIGNAL_BUFFER_SIZE * sizeof(int16_t) * 2);

#if 0 // TODO: parametrize this
    if (radio_type == RADIO_SBITX)
        ch_layout = LEFT;
    if (radio_type == RADIO_STOCKHF)
        ch_layout = STEREO;
#endif
    ch_layout = STEREO;

    while (!shutdown_)
    {
        r = audio->read(b, (const void **)&buffer);
        if (r < 0)
        {
            printf("ffaudio.read: %s", audio->error(b));
            continue;
        }
#if 0
        else
        {
            printf(" %dms\n", r / msec_bytes);
        }
#endif

        int frames_read = r / frame_size;
        int frames_to_write = frames_read;

        for (int i = 0; i < frames_to_write; i++)
        {
            if (ch_layout == LEFT)
            {
                buffer_output[i] = (int16_t) buffer[i*2] >> 16;
            }

            if (ch_layout == RIGHT)
            {
                buffer_output[i] = (int16_t) buffer[i*2 + 1] >> 16;
            }

            if (ch_layout == STEREO)
            {
                buffer_output[i] = (int16_t) ((buffer[i*2] + buffer[i*2 + 1]) / 2) >> 16;
            }
        }

        if (circular_buf_free_size(capture_buffer) >= frames_to_write * sizeof(int16_t))
            write_buffer(capture_buffer, (uint8_t *)buffer_output, frames_to_write * sizeof(int16_t));
        else
            printf("Buffer full in capture buffer!\n");
    }

    r = audio->stop(b);
    if (r != 0)
        printf("ffaudio.stop: %s", audio->error(b));

    r = audio->clear(b);
    if (r != 0)
        printf("ffaudio.clear: %s", audio->error(b));

    free(buffer_output);

cleanup_cap:

    audio->free(b);

    audio->uninit();

finish_cap:
    printf("radio_capture_thread exit\n");

    shutdown_ = true;

    return NULL;
}

void list_soundcards(int audio_system)
{
    ffaudio_interface *audio;
    audio_subsystem = audio_system;

    if (audio_subsystem == AUDIO_SUBSYSTEM_SHM)
    {
        // TODO: connect to the shared memory
        printf("Shared Memory (SHM) audio subsystem selected.\n");
        audio = NULL;
        return;
    }
    
#if defined(_WIN32)
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
    {
        printf("Listing ALSA soundcards:\n");
        audio = (ffaudio_interface *) &ffalsa;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#elif defined(__ANDROID__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_AAUDIO)
        audio = (ffaudio_interface *) &ffaaudio;
#endif

    ffaudio_init_conf aconf = {};
    if ( audio->init(&aconf) != 0)
    {
        printf("Error in audio->init()\n");
        return;
    }

    ffaudio_dev *d;

    // FFAUDIO_DEV_PLAYBACK, FFAUDIO_DEV_CAPTURE
    static const char* const mode[] = { "playback", "capture" };
    for (ffuint i = 0;  i != 2;  i++)
    {
        printf("%s devices:\n", mode[i]);
        d = audio->dev_alloc(i);
        if (d == NULL)
        {
            printf("Error in audio->dev_alloc\n");
            return;
        }

        for (;;)
        {
            int r = audio->dev_next(d);
            if (r > 0)
                break;
            else
                if (r < 0)
                {
                    printf("error: %s", audio->dev_error(d));
                    break;
                }

            printf("device: name: '%s'  id: '%s'  default: %s\n"
                   , audio->dev_info(d, FFAUDIO_DEV_NAME)
                   , audio->dev_info(d, FFAUDIO_DEV_ID)
                   , audio->dev_info(d, FFAUDIO_DEV_IS_DEFAULT)
                );
        }

        audio->dev_free(d);
    }
}

#if 0
// size in "double" samples
int tx_transfer(double *buffer, size_t len)
{
    uint8_t *buffer_internal = (uint8_t *) buffer;
    int buffer_size_bytes = len * sizeof(double);

    write_buffer(playback_buffer, buffer_internal, buffer_size_bytes);

    // printf("size %llu free %llu\n", size_buffer(playback_buffer), circular_buf_free_size(playback_buffer));

    return 0;
}

// size in "double" samples
int rx_transfer(double *buffer, size_t len)
{
    uint8_t *buffer_internal = (uint8_t *) buffer;
    int buffer_size_bytes = len * sizeof(double);

    read_buffer(capture_buffer, buffer_internal, buffer_size_bytes);

    return 0;
}
#endif

int audioio_init_internal(char *capture_dev, char *playback_dev, int audio_subsys, pthread_t *radio_capture,
                          pthread_t *radio_playback)
{
    audio_subsystem = audio_subsys;

#if defined(_WIN32)
    uint8_t *buffer_cap = (uint8_t *)malloc(SIGNAL_BUFFER_SIZE);
    uint8_t *buffer_play = (uint8_t *)malloc(SIGNAL_BUFFER_SIZE);
    capture_buffer = circular_buf_init(buffer_cap, SIGNAL_BUFFER_SIZE);
    playback_buffer = circular_buf_init(buffer_play, SIGNAL_BUFFER_SIZE);
#else
    capture_buffer = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
    playback_buffer = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
#endif

    clear_buffer(capture_buffer);
    clear_buffer(playback_buffer);

    pthread_create(radio_capture, NULL, radio_capture_thread, (void *) capture_dev);
    pthread_create(radio_playback, NULL, radio_playback_thread, (void *) playback_dev);

    return 0;
}

int audioio_deinit(pthread_t *radio_capture, pthread_t *radio_playback)
{
    pthread_join(*radio_capture, NULL);
    pthread_join(*radio_playback, NULL);

#if defined(_WIN32)
    free(capture_buffer->buffer);
    circular_buf_free(capture_buffer);
    free(playback_buffer->buffer);
    circular_buf_free(playback_buffer);
#else
    circular_buf_destroy_shm(capture_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
    circular_buf_free_shm(capture_buffer);

    circular_buf_destroy_shm(playback_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
    circular_buf_free_shm(playback_buffer);
#endif
    return 0;
}

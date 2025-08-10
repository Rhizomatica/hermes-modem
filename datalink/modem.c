/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>

#include "modem.h"
#include "ring_buffer_posix.h"

#include "freedv_api.h"
#include "fsk.h"
#include "ldpc_codes.h"
#include "ofdm_internal.h"

#include "defines.h"

extern bool shutdown_; // global shutdown flag

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

void *radio_shm_playback_thread(void *freedv_ptr)
{
    while(!shutdown_)
    {

    }

    return NULL;
}

void *radio_shm_capture_thread(void *freedv_ptr)
{
    while(!shutdown_)
    {

    }

    return NULL;
}


int init_modem(struct freedv **freedv, int mode, int frames_per_burst, pthread_t *radio_capture, pthread_t *radio_playback)
{
    // connect to shared memory buffers
try_shm_connect1:
    capture_buffer = circular_buf_connect_shm(SIGNAL_BUFFER_SIZE, SIGNAL_INPUT);
    if (capture_buffer == NULL)
    {
        printf("Shared memory not created. Waiting for the radio daemon\n");
        sleep(2);
        goto try_shm_connect1;
    }

try_shm_connect2:
    playback_buffer = circular_buf_connect_shm(SIGNAL_BUFFER_SIZE, SIGNAL_OUTPUT);
    if (playback_buffer == NULL)
    {
        printf("Shared memory not created. Waiting for the radio daemon...\n");
        sleep(2);
        goto try_shm_connect2;
    }
    printf("Connected to shared memory buffers.\n");
    
    char codename[80] = "H_256_512_4";
    struct freedv_advanced adv = {0, 2, 100, 8000, 1000, 200, codename};

    if (mode == FREEDV_MODE_FSK_LDPC)
        *freedv = freedv_open_advanced(mode, &adv);
    else 
        *freedv = freedv_open(mode);
    
    freedv_set_frames_per_burst(*freedv, frames_per_burst);

    pthread_create(radio_capture, NULL, radio_shm_capture_thread, (void *) *freedv);
    pthread_create(radio_playback, NULL, radio_shm_playback_thread, (void *) *freedv);
    
    return 0;
}

int shutdown_modem(struct freedv *freedv, pthread_t *radio_capture, pthread_t *radio_playback)
{
    pthread_join(*radio_capture, NULL);
    pthread_join(*radio_playback, NULL);

    circular_buf_destroy_shm(capture_buffer, SIGNAL_BUFFER_SIZE, SIGNAL_INPUT);
    circular_buf_destroy_shm(playback_buffer, SIGNAL_BUFFER_SIZE, SIGNAL_OUTPUT);
    circular_buf_free_shm(capture_buffer);
    circular_buf_free_shm(playback_buffer);
    
    freedv_close(freedv);   

    return 0;
}

int send_modulated_data(struct freedv *freedv, uint8_t *bytes_in, int frames_per_burst)
{
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    size_t payload_bytes_per_modem_frame = bytes_per_modem_frame - 2; /* 16 bits used for the CRC */
    size_t n_mod_out = freedv_get_n_tx_modem_samples(freedv);
    int16_t mod_out_short[n_mod_out];
    int32_t mod_out_int32[n_mod_out];

    /* send preamble */
    int n_preamble = freedv_rawdatapreambletx(freedv, mod_out_short);
    // converting from s16le to s32le
    for (int i = 0; i < n_preamble; i++)
    {
        mod_out_int32[i] = (int32_t)mod_out_short[i] << 16;
    }
    write_buffer(playback_buffer, (uint8_t *) mod_out_int32, sizeof(int32_t) * n_preamble);

    /* The raw data modes require a CRC in the last two bytes */
    uint16_t crc16 = freedv_gen_crc16(bytes_in, payload_bytes_per_modem_frame);
    bytes_in[bytes_per_modem_frame - 2] = crc16 >> 8;
    bytes_in[bytes_per_modem_frame - 1] = crc16 & 0xff;

    /* modulate and send a data frame */
    for (int i = 0; i < frames_per_burst; i++)
    {
        freedv_rawdatatx(freedv, mod_out_short, &bytes_in[bytes_per_modem_frame * i]);
        // converting from s16le to s32le
        for (int j = 0; j < n_mod_out; j++)
        {
            mod_out_int32[j] = (int32_t)mod_out_short[j] << 16;
        }
        write_buffer(playback_buffer, (uint8_t *) mod_out_short, sizeof(int32_t) * n_mod_out);
    }
    /* send postamble */
    int n_postamble = freedv_rawdatapostambletx(freedv, mod_out_short);
    // converting from s16le to s32le
    for (int i = 0; i < n_postamble; i++)
    {
        mod_out_int32[i] = (int32_t)mod_out_short[i] << 16;
    }
    write_buffer(playback_buffer, (uint8_t *) mod_out_int32, sizeof(int32_t) * n_postamble);

    /* create some silence between bursts */
    int inter_burst_delay_ms = 200;
    int samples_delay = FREEDV_FS_8000 * inter_burst_delay_ms / 1000;
    int32_t silence[samples_delay];
    memset(silence, 0, samples_delay * sizeof(int32_t));
    write_buffer(playback_buffer, (uint8_t *) silence, sizeof(int32_t) * samples_delay);
    
    return 0;
}

int receive_modulated_data(struct freedv *freedv, uint8_t *bytes_out, size_t *nbytes_out)
{
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    size_t payload_bytes_per_modem_frame = bytes_per_modem_frame - 2; /* 16 bits used for the CRC */

    static int frames_received = 0;
    int input_size = freedv_get_n_max_modem_samples(freedv);

    // TODO: read from buffer
    int16_t demod_in[input_size];
    int32_t buffer_in[input_size];
    
    size_t nin = freedv_nin(freedv);
    read_buffer(capture_buffer, (uint8_t *) buffer_in, sizeof(int32_t) * nin);
    // converting from s32le to s16le
    for (size_t i = 0; i < nin; i++)
    {
        demod_in[i] = (int16_t)(buffer_in[i] >> 16);
    }
    
    *nbytes_out = freedv_rawdatarx(freedv, bytes_out, demod_in);

    // check crc
    if (*nbytes_out > 0)
    {
        uint16_t crc16 = freedv_gen_crc16(bytes_out, payload_bytes_per_modem_frame);
        uint16_t received_crc16 = (bytes_out[*nbytes_out - 2] << 8) | bytes_out[*nbytes_out - 1];
        if (crc16 != received_crc16)
        {
            fprintf(stderr, "CRC error in received frame\n");
            *nbytes_out = 0; // reset output size on CRC error
            return -1;
        }
        else
        {
            fprintf(stderr, "CRC check passed\n");
            // remove CRC from output
            *nbytes_out -= 2;
        }

        frames_received++;
        int sync = 0;
        float snr_est = 0.0;
        freedv_get_modem_stats(freedv, &sync, &snr_est);
        printf("Received %d frames, SNR: %.2f dB, sync: %d\n", frames_received, snr_est, sync);
    }

    return 0;
}

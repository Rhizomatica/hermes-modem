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
#include "framer.h"
#include "tcp_interfaces.h"
#include "freedv_api.h"
#include "fsk.h"
#include "ldpc_codes.h"
#include "ofdm_internal.h"

#include "defines_modem.h"

/* Here we read and write from the input and output buffers which are connected */
/* to the radio dsp paths. */

/* The buffers can be connected to an external software (eg. sbitx_controller)  */
/* or to the audioio subsystem which connects to a sound card. */

extern bool shutdown_; // global shutdown flag

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

extern char *freedv_mode_names[];

cbuf_handle_t data_tx_buffer_arq;
cbuf_handle_t data_rx_buffer_arq;

cbuf_handle_t data_tx_buffer_broadcast;
cbuf_handle_t data_rx_buffer_broadcast;

pthread_t tx_thread_tid, rx_thread_tid;

int init_modem(generic_modem_t *g_modem, int mode, int frames_per_burst, int test_mode)
{
// connect to shared memory buffers
try_shm_connect1:
    capture_buffer = circular_buf_connect_shm(SIGNAL_BUFFER_SIZE, SIGNAL_INPUT);
    if (capture_buffer == NULL)
    {
        printf("Shared memory not created. Waiting for the radio daemon\n");
        sleep(1);
        goto try_shm_connect1;
    }

try_shm_connect2:
    playback_buffer = circular_buf_connect_shm(SIGNAL_BUFFER_SIZE, SIGNAL_OUTPUT);
    if (playback_buffer == NULL)
    {
        printf("Shared memory not created. Waiting for the radio daemon...\n");
        sleep(1);
        goto try_shm_connect2;
    }
    printf("Connected to Shared Memory Radio I/O tx/rx buffers.\n");

    // buffers for the ARQ datalink
    uint8_t *buffer_tx = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    uint8_t *buffer_rx = (uint8_t *) malloc(DATA_RX_BUFFER_SIZE);
    data_tx_buffer_arq = circular_buf_init(buffer_tx, DATA_TX_BUFFER_SIZE);
    data_rx_buffer_arq = circular_buf_init(buffer_rx, DATA_RX_BUFFER_SIZE);

    // buffers for the broadcast datalink
    buffer_tx = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    buffer_rx = (uint8_t *) malloc(DATA_RX_BUFFER_SIZE);
    data_tx_buffer_broadcast = circular_buf_init(buffer_tx, DATA_TX_BUFFER_SIZE);
    data_rx_buffer_broadcast = circular_buf_init(buffer_rx, DATA_RX_BUFFER_SIZE);
    
    printf("Created data buffers for ARQ and BROADCAST datalink, tx/rx paths.\n");

    char codename[80] = "H_256_512_4";
    struct freedv_advanced adv = {0, 2, 100, 8000, 1000, 200, codename};

    if (mode == FREEDV_MODE_FSK_LDPC)
        g_modem->freedv = freedv_open_advanced(mode, &adv);
    else 
        g_modem->freedv = freedv_open(mode);
    
    freedv_set_frames_per_burst(g_modem->freedv, frames_per_burst);

    freedv_set_verbose(g_modem->freedv, 3);
    
    int modem_sample_rate = freedv_get_modem_sample_rate(g_modem->freedv);
    printf("Opened FreeDV modem with mode %d (%s), frames per burst: %d, verbosity: %d\n", mode, freedv_mode_names[mode], frames_per_burst, 3);
    printf("Modem expects sample rate: %d Hz\n", modem_sample_rate);
    
    if (modem_sample_rate != 8000)
    {
        printf("WARNING: Modem sample rate is %d Hz, but audio I/O is configured for 8kHz!\n", modem_sample_rate);
        printf("         You need to adjust the resampling in audioio.c or use a different mode.\n");
    }

    // test if testing is enable
    if(test_mode == 1) // tx
    {
        run_tests_tx(g_modem);
    }
    if(test_mode == 2) // rx
    {
        run_tests_rx(g_modem);
    }

    // Create TX and RX threads
    pthread_create(&tx_thread_tid, NULL, tx_thread, (void *)g_modem);
    pthread_create(&rx_thread_tid, NULL, rx_thread, (void *)g_modem);

    return 0;
}

int run_tests_tx(generic_modem_t *g_modem)
{
    size_t frame_size = freedv_get_bits_per_modem_frame(g_modem->freedv) / 8;
    uint8_t *buffer = (uint8_t *)malloc(frame_size);

    int counter = 0;

    while(1)
    {
        for (size_t i = 0; i < frame_size; i++)
        {
            buffer[i] = 0;
        }
        buffer[counter % frame_size] = 1;
        counter++;

        send_modulated_data(g_modem, buffer, 1);

        // lets not forget to discard the input buffer... as we are just transmitting
        size_t rx_discard = size_buffer(capture_buffer);
        if (rx_discard > 0)
        {
            clear_buffer(capture_buffer);
        }
    }

    free(buffer);
    return 0;
}


int run_tests_rx(generic_modem_t *g_modem)
{
    size_t frame_size = freedv_get_bits_per_modem_frame(g_modem->freedv) / 8;
    uint8_t *buffer = (uint8_t *)malloc(frame_size);

    size_t bytes_out = 0;
    int counter = 0;
    
    while(1)
    {
        receive_modulated_data(g_modem, buffer, &bytes_out);
        if (bytes_out == 0)
            continue;

        counter++;
        int sync = 0;
        float snr_est = 0.0;
        freedv_get_modem_stats(g_modem->freedv, &sync, &snr_est);
        printf("Received %d frames, SNR: %.2f dB, sync: %d\n", counter, snr_est, sync);

        for (size_t j = 0; j < bytes_out; j++)
        {
            putchar(buffer[j] + '0');
        }
        printf("\n");
    }

    free(buffer);
    return 0;
}

int shutdown_modem(generic_modem_t *g_modem)
{
    // Wait for threads to finish
    pthread_join(tx_thread_tid, NULL);
    pthread_join(rx_thread_tid, NULL);
    
    circular_buf_disconnect_shm(capture_buffer, SIGNAL_BUFFER_SIZE);
    circular_buf_disconnect_shm(playback_buffer, SIGNAL_BUFFER_SIZE);
    circular_buf_free_shm(capture_buffer);
    circular_buf_free_shm(playback_buffer);

    free(data_tx_buffer_arq->buffer);
    free(data_rx_buffer_arq->buffer);
    free(data_tx_buffer_broadcast->buffer);
    free(data_rx_buffer_broadcast->buffer);
    circular_buf_free(data_tx_buffer_arq);
    circular_buf_free(data_rx_buffer_arq);
    circular_buf_free(data_tx_buffer_broadcast);
    circular_buf_free(data_rx_buffer_broadcast);
    
    freedv_close(g_modem->freedv);   

    return 0;
}

int send_modulated_data(generic_modem_t *g_modem, uint8_t *bytes_in, int frames_per_burst)
{
    struct freedv *freedv = g_modem->freedv;
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    size_t n_mod_out = freedv_get_n_tx_modem_samples(freedv);
    int16_t mod_out_short[n_mod_out];
    int32_t mod_out_int32[n_mod_out];
    int total_samples = 0;
    
    /* send preamble */
    int n_preamble = freedv_rawdatapreambletx(freedv, mod_out_short);
    // converting from s16le to s32le
    for (int i = 0; i < n_preamble; i++)
    {
        mod_out_int32[i] = (int32_t)mod_out_short[i] << 16;
    }
    write_buffer(playback_buffer, (uint8_t *) mod_out_int32, sizeof(int32_t) * n_preamble);
    total_samples += n_preamble;

    /* modulate and send a data frame */
    for (int i = 0; i < frames_per_burst; i++)
    {
        freedv_rawdatatx(freedv, mod_out_short, &bytes_in[bytes_per_modem_frame * i]);
        // converting from s16le to s32le
        for (int j = 0; j < n_mod_out; j++)
        {
            mod_out_int32[j] = (int32_t)mod_out_short[j] << 16;
        }
        write_buffer(playback_buffer, (uint8_t *) mod_out_int32, sizeof(int32_t) * n_mod_out);
        total_samples += n_mod_out;
    }
    /* send postamble */
    int n_postamble = freedv_rawdatapostambletx(freedv, mod_out_short);
    // converting from s16le to s32le
    for (int i = 0; i < n_postamble; i++)
    {
        mod_out_int32[i] = (int32_t)mod_out_short[i] << 16;
    }
    write_buffer(playback_buffer, (uint8_t *) mod_out_int32, sizeof(int32_t) * n_postamble);
    total_samples += n_postamble;
    
    /* create some silence between bursts */
    int inter_burst_delay_ms = 200;
    int samples_delay = FREEDV_FS_8000 * inter_burst_delay_ms / 1000;
    int32_t silence[samples_delay];
    memset(silence, 0, samples_delay * sizeof(int32_t));
    write_buffer(playback_buffer, (uint8_t *) silence, sizeof(int32_t) * samples_delay);
    total_samples += samples_delay;

    ptt_on();
    
    usleep(total_samples * 1000000 / FREEDV_FS_8000); // wait for the samples to be played

    // give some more tail time just to be in the safe side
    usleep(TAIL_TIME_US);

    ptt_off();
    
    return 0;
}

int receive_modulated_data(generic_modem_t *g_modem, uint8_t *bytes_out, size_t *nbytes_out)
{
    struct freedv *freedv = g_modem->freedv;

    static int frames_received = 0;
    static int debug_counter = 0;
    static int16_t *demod_in = NULL;
    static int32_t *buffer_in = NULL;
    static int buffer_size = 0;

    int input_size = freedv_get_n_max_modem_samples(freedv);
    
    // Allocate buffers on first call or if size changed
    if (buffer_size < input_size)
    {
        if (demod_in) free(demod_in);
        if (buffer_in) free(buffer_in);
        demod_in = (int16_t *)malloc(input_size * sizeof(int16_t));
        buffer_in = (int32_t *)malloc(input_size * sizeof(int32_t));
        buffer_size = input_size;
    }
    
    size_t nin = freedv_nin(freedv);

    // Safety check: nin should not be 0 or larger than buffer
    if (nin == 0 || nin > (size_t)input_size)
    {
        printf("[DEBUG RX] WARNING: nin=%zu (input_size=%d), skipping\n", nin, input_size);
        *nbytes_out = 0;
        return 0;
    }

    read_buffer(capture_buffer, (uint8_t *) buffer_in, sizeof(int32_t) * nin);
    
    // Debug: print signal level every ~1 second (8000 samples/sec)
    debug_counter += nin;
    if (debug_counter >= 8000)
    {
        int32_t max_val = 0;
        int32_t min_val = 0;
        for (size_t i = 0; i < nin; i++)
        {
            if (buffer_in[i] > max_val) max_val = buffer_in[i];
            if (buffer_in[i] < min_val) min_val = buffer_in[i];
        }
        printf("[DEBUG RX] buffer size: %zu, nin: %zu, signal range: [%d, %d]\n", 
               size_buffer(capture_buffer), nin, min_val, max_val);
        debug_counter = 0;
    }
    
    // converting from s32le to s16le
    for (size_t i = 0; i < nin; i++)
    {
        demod_in[i] = (int16_t)(buffer_in[i] >> 16);
    }
    
    *nbytes_out = freedv_rawdatarx(freedv, bytes_out, demod_in);

    if (*nbytes_out > 0)
    {
        frames_received++;
        int sync = 0;
        float snr_est = 0.0;
        freedv_get_modem_stats(freedv, &sync, &snr_est);
        printf("Received %d frames, SNR: %.2f dB, sync: %d\n", frames_received, snr_est, sync);
    }

    return 0;
}


// Threads


// Function to handle TX logic for the broadcast tx path
void *tx_thread(void *g_modem)
{
    struct freedv *freedv = ((generic_modem_t *)g_modem)->freedv;
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    int frames_per_burst = freedv_get_frames_per_burst(freedv);
    uint8_t *data = (uint8_t *)malloc(bytes_per_modem_frame * frames_per_burst);

    if (!data)
    {
        fprintf(stderr, "Failed to allocate memory for TX data.\n");
        return NULL;
    }

    while (!shutdown_)
    {
        if (size_buffer(data_tx_buffer_arq) >= bytes_per_modem_frame * frames_per_burst)
        {
            for (int i = 0; i < frames_per_burst; i++)
            {
                read_buffer(data_tx_buffer_arq, data + (bytes_per_modem_frame * i), bytes_per_modem_frame);
            }
            send_modulated_data(g_modem, data, frames_per_burst);
        }

        if (size_buffer(data_tx_buffer_broadcast) >= bytes_per_modem_frame * frames_per_burst)
        {
            for (int i = 0; i < frames_per_burst; i++)
            {
                read_buffer(data_tx_buffer_broadcast, data + (bytes_per_modem_frame * i), bytes_per_modem_frame);
            }
            send_modulated_data(g_modem, data, frames_per_burst);
        }

        if (size_buffer(data_tx_buffer_arq) < bytes_per_modem_frame * frames_per_burst &&
            size_buffer(data_tx_buffer_broadcast) < bytes_per_modem_frame * frames_per_burst)
        {
            usleep(100000); // sleep for 100ms if there is no data to send
        }
    }

    free(data);
    return NULL;
}

// Function to handle RX logic for the broadcast rx path
void *rx_thread(void *g_modem)
{
    struct freedv *freedv = ((generic_modem_t *)g_modem)->freedv;
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    int frames_per_burst = freedv_get_frames_per_burst(freedv);
    uint8_t *data = (uint8_t *)malloc(bytes_per_modem_frame * frames_per_burst);
    size_t nbytes_out = 0;

    if (!data)
    {
        fprintf(stderr, "Failed to allocate memory for RX data.\n");
        return NULL;
    }

    while (!shutdown_)
    {
        // we are running full-duplex for now - TODO: change to half-duplex
        receive_modulated_data(g_modem, data, &nbytes_out);
        if (nbytes_out > 0)
        {
            int frame_type = parse_frame_header(data, nbytes_out);

            switch (frame_type)
            {
            case PACKET_TYPE_ARQ_CONTROL:
            case PACKET_TYPE_ARQ_DATA:
                write_buffer(data_rx_buffer_arq, data, nbytes_out);
                break;
            case PACKET_TYPE_BROADCAST_CONTROL:
            case PACKET_TYPE_BROADCAST_DATA:
                write_buffer(data_rx_buffer_broadcast, data, nbytes_out);
                break;
            default:
                printf("Unknown frame type received.\n");
                break;
            }
            
            printf("Received %zu bytes of data, packet type %d, bytes_per_modem_frame: %zu\n", nbytes_out, frame_type, bytes_per_modem_frame);
        }
    }

    free(data);
    return NULL;
}

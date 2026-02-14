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
#include "arq.h"
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
static pthread_mutex_t modem_freedv_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *mode_name_from_enum(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1: return "DATAC1";
    case FREEDV_MODE_DATAC3: return "DATAC3";
    case FREEDV_MODE_DATAC0: return "DATAC0";
    case FREEDV_MODE_DATAC4: return "DATAC4";
    case FREEDV_MODE_DATAC13: return "DATAC13";
    case FREEDV_MODE_DATAC14: return "DATAC14";
    case FREEDV_MODE_FSK_LDPC: return "FSK_LDPC";
    default: return "UNKNOWN";
    }
}

static struct freedv *open_freedv_mode_locked(int mode)
{
    char codename[80] = "H_256_512_4";
    struct freedv_advanced adv = {0, 2, 100, 8000, 1000, 200, codename};

    if (mode == FREEDV_MODE_FSK_LDPC)
        return freedv_open_advanced(mode, &adv);
    return freedv_open(mode);
}

static uint32_t compute_bitrate_bps_locked(struct freedv *freedv)
{
    uint32_t bits_per_modem_frame = (uint32_t)freedv_get_bits_per_modem_frame(freedv);
    uint32_t tx_modem_samples = (uint32_t)freedv_get_n_tx_modem_samples(freedv);
    uint32_t modem_sample_rate = (uint32_t)freedv_get_modem_sample_rate(freedv);

    if (tx_modem_samples == 0)
        return 0;

    return (uint32_t)(((uint64_t)bits_per_modem_frame * modem_sample_rate + (tx_modem_samples / 2)) / tx_modem_samples);
}

static int maybe_switch_modem_mode(generic_modem_t *g_modem, int target_mode)
{
    if (target_mode < 0)
        return 0;
    if (arq_conn.TRX == TX)
        return 0;
    if (size_buffer(data_tx_buffer_arq) > 0 || size_buffer(data_tx_buffer_broadcast) > 0)
        return 0;

    pthread_mutex_lock(&modem_freedv_lock);
    int current_mode = g_modem->mode;
    pthread_mutex_unlock(&modem_freedv_lock);
    if (current_mode == target_mode)
        return 0;

    struct freedv *new_freedv = open_freedv_mode_locked(target_mode);
    if (!new_freedv)
        return -1;
    freedv_set_frames_per_burst(new_freedv, 1);
    freedv_set_verbose(new_freedv, 0);

    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(new_freedv) / 8;
    size_t payload_bytes_per_modem_frame = bytes_per_modem_frame - 2;

    pthread_mutex_lock(&modem_freedv_lock);
    struct freedv *old_freedv = g_modem->freedv;
    g_modem->freedv = new_freedv;
    g_modem->mode = target_mode;
    g_modem->payload_bytes_per_modem_frame = payload_bytes_per_modem_frame;
    pthread_mutex_unlock(&modem_freedv_lock);
    arq_set_active_modem_mode(target_mode, payload_bytes_per_modem_frame);

    if (old_freedv)
        freedv_close(old_freedv);

    fprintf(stderr, "Switched modem mode to %d (%s), payload=%zu\n",
            target_mode, mode_name_from_enum(target_mode), payload_bytes_per_modem_frame);
    return 1;
}

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

    g_modem->freedv = open_freedv_mode_locked(mode);
    if (!g_modem->freedv)
    {
        fprintf(stderr, "Failed to open FreeDV mode %d\n", mode);
        return -1;
    }
    
    freedv_set_frames_per_burst(g_modem->freedv, frames_per_burst);

    freedv_set_verbose(g_modem->freedv, 0);

    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(g_modem->freedv) / 8;
    size_t payload_bytes_per_modem_frame = bytes_per_modem_frame - 2;
    g_modem->mode = mode;
    g_modem->payload_bytes_per_modem_frame = payload_bytes_per_modem_frame;
    
    int modem_sample_rate = freedv_get_modem_sample_rate(g_modem->freedv);
    printf("Opened FreeDV modem with mode %d (%s), frames per burst: %d, verbosity: %d\n", mode, mode_name_from_enum(mode), frames_per_burst, 0);
    printf("Modem expects sample rate: %d Hz\n", modem_sample_rate);
    printf("Modem payload bytes per frame: %zu\n", payload_bytes_per_modem_frame);
    printf("Split control/data mode switching: ENABLED\n");
    
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
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(g_modem->freedv) / 8;
    size_t payload_size = bytes_per_modem_frame - 2;  /* 2 bytes reserved for CRC by send_modulated_data */
    uint8_t *buffer = (uint8_t *)malloc(payload_size);

    if (!buffer)
    {
        printf("ERROR: Failed to allocate TX test buffer\n");
        return -1;
    }

    printf("TX test: bytes_per_modem_frame=%zu, payload_size=%zu\n",
           bytes_per_modem_frame, payload_size);

    int counter = 0;

    while(1)
    {
        for (size_t i = 0; i < payload_size; i++)
        {
            buffer[i] = 0;
        }
        buffer[counter % payload_size] = 1;
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
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(g_modem->freedv) / 8;
    size_t payload_size = bytes_per_modem_frame - 2;  /* RX returns frame with CRC, payload is 2 bytes less */
    uint8_t *buffer = (uint8_t *)malloc(bytes_per_modem_frame);

    if (!buffer)
    {
        printf("ERROR: Failed to allocate RX test buffer\n");
        return -1;
    }

    printf("RX test: bytes_per_modem_frame=%zu, payload_size=%zu\n",
           bytes_per_modem_frame, payload_size);

    size_t bytes_out = 0;
    int counter = 0;

    while(1)
    {
        receive_modulated_data(g_modem, buffer, &bytes_out);
        if (bytes_out == 0)
            continue;

        counter++;
        /* bytes_out includes CRC, actual payload is bytes_out - 2 */
        size_t payload_len = (bytes_out >= 2) ? bytes_out - 2 : bytes_out;

        printf("Frame %d (%zu payload bytes):\n", counter, payload_len);
        for (size_t j = 0; j < payload_len; j++)
        {
            printf("%02x ", buffer[j]);
            if ((j + 1) % 16 == 0) printf("\n");
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
    pthread_mutex_lock(&modem_freedv_lock);
    struct freedv *freedv = g_modem->freedv;
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    size_t payload_bytes = bytes_per_modem_frame - 2;  /* 2 bytes reserved for CRC16 */
    size_t n_mod_out = freedv_get_n_tx_modem_samples(freedv);
    uint8_t frame_with_crc[bytes_per_modem_frame];

    /* Inter-burst silence */
    int inter_burst_delay_ms = 200;
    int samples_silence = FREEDV_FS_8000 * inter_burst_delay_ms / 1000;

    /* Calculate max buffer size needed:
     * preamble + (frames * n_mod_out) + postamble + silence */
    int max_preamble = freedv_get_n_tx_modem_samples(freedv) * 2;  /* conservative estimate */
    int max_postamble = max_preamble;
    size_t max_samples = max_preamble + (frames_per_burst * n_mod_out) + max_postamble + samples_silence;

    /* Allocate temporary buffer for all modulated audio */
    int32_t *tx_buffer = (int32_t *)malloc(max_samples * sizeof(int32_t));
    int16_t *mod_out_short = (int16_t *)malloc(n_mod_out * sizeof(int16_t));

    if (!tx_buffer || !mod_out_short)
    {
        printf("ERROR: Failed to allocate TX buffer\n");
        if (tx_buffer) free(tx_buffer);
        if (mod_out_short) free(mod_out_short);
        pthread_mutex_unlock(&modem_freedv_lock);
        return -1;
    }

    int total_samples = 0;


    /* === STEP 1: Generate all modulated audio into temp buffer === */

    /* Generate preamble */
    int n_preamble = freedv_rawdatapreambletx(freedv, mod_out_short);
    for (int i = 0; i < n_preamble; i++)
    {
        tx_buffer[total_samples++] = (int32_t)mod_out_short[i] << 16;
    }

    /* Generate data frame(s) */
    for (int i = 0; i < frames_per_burst; i++)
    {
        /* Copy payload and add CRC16 in last 2 bytes */
        memcpy(frame_with_crc, &bytes_in[payload_bytes * i], payload_bytes);
        uint16_t crc16 = freedv_gen_crc16(frame_with_crc, payload_bytes);
        frame_with_crc[bytes_per_modem_frame - 2] = crc16 >> 8;
        frame_with_crc[bytes_per_modem_frame - 1] = crc16 & 0xff;

        freedv_rawdatatx(freedv, mod_out_short, frame_with_crc);
        for (size_t j = 0; j < n_mod_out; j++)
        {
            tx_buffer[total_samples++] = (int32_t)mod_out_short[j] << 16;
        }
    }

    /* Generate postamble */
    int n_postamble = freedv_rawdatapostambletx(freedv, mod_out_short);
    for (int i = 0; i < n_postamble; i++)
    {
        tx_buffer[total_samples++] = (int32_t)mod_out_short[i] << 16;
    }

    /* Add silence at end */
    for (int i = 0; i < samples_silence; i++)
    {
        tx_buffer[total_samples++] = 0;
    }
    pthread_mutex_unlock(&modem_freedv_lock);


    /* === STEP 2: Key transmitter and send pre-generated audio === */

    ptt_on();
    
    /* Wait for radio relay to switch (10ms for your radio) */
    usleep(10000);

    /* Write entire pre-generated buffer to playback */
    write_buffer(playback_buffer, (uint8_t *)tx_buffer, total_samples * sizeof(int32_t));

    /* Wait for all samples to be played out */
    usleep(total_samples * 1000000 / FREEDV_FS_8000);

    /* Give some tail time before turning off PTT */
    usleep(TAIL_TIME_US);

    ptt_off();

    free(tx_buffer);
    free(mod_out_short);

    return 0;
}

int receive_modulated_data(generic_modem_t *g_modem, uint8_t *bytes_out, size_t *nbytes_out)
{
    pthread_mutex_lock(&modem_freedv_lock);
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

    // Safety check: nin should not be larger than buffer
    if (nin > (size_t)input_size)
    {
        printf("[DEBUG RX] ERROR: nin=%zu exceeds input_size=%d\n", nin, input_size);
        *nbytes_out = 0;
        pthread_mutex_unlock(&modem_freedv_lock);
        return -1;
    }

    // Read samples only when nin > 0
    // When nin == 0, FreeDV has buffered enough samples and will process internally
    if (nin > 0)
    {
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
    }

    // ALWAYS call freedv_rawdatarx - even when nin==0, it processes internal buffers
    *nbytes_out = freedv_rawdatarx(freedv, bytes_out, demod_in);

    // Check modem state and RX status
    int sync = 0;
    float snr_est = 0.0;
    freedv_get_modem_stats(freedv, &sync, &snr_est);
    int rx_status = freedv_get_rx_status(freedv);

    // Debug: show RX status flags
    // FREEDV_RX_SYNC = 0x1, FREEDV_RX_BITS = 0x2, FREEDV_RX_BIT_ERRORS = 0x4
    static int sync_count = 0;
    static int last_rx_status = 0;

    if (rx_status != last_rx_status || (sync && sync_count % 50 == 0))
    {
        printf("[DEBUG RX STATUS] rx_status=0x%x (SYNC=%d BITS=%d BIT_ERRORS=%d) sync=%d snr=%.1f nbytes=%zu\n",
               rx_status,
               (rx_status & 0x1) ? 1 : 0,   // FREEDV_RX_SYNC
               (rx_status & 0x2) ? 1 : 0,   // FREEDV_RX_BITS
               (rx_status & 0x4) ? 1 : 0,   // FREEDV_RX_BIT_ERRORS
               sync, snr_est, *nbytes_out);
        last_rx_status = rx_status;
    }

    if (sync)
        sync_count++;
    else
        sync_count = 0;

    if (*nbytes_out > 0)
    {
        frames_received++;
        printf(">>> DECODED FRAME %d: %zu bytes, SNR: %.2f dB\n",
               frames_received, *nbytes_out, snr_est);
    }

    pthread_mutex_unlock(&modem_freedv_lock);
    return 0;
}


// Threads


// Function to handle TX logic for the broadcast tx path
void *tx_thread(void *g_modem)
{
    generic_modem_t *modem = (generic_modem_t *)g_modem;
    uint8_t *data = NULL;
    size_t data_size = 0;

    while (!shutdown_)
    {
        if (arq_conn.TRX != TX)
        {
            int pref_tx_mode = arq_get_preferred_tx_mode();
            if (pref_tx_mode >= 0)
                maybe_switch_modem_mode(modem, pref_tx_mode);
        }

        size_t payload_bytes_per_modem_frame = 0;
        int frames_per_burst = 1;
        pthread_mutex_lock(&modem_freedv_lock);
        payload_bytes_per_modem_frame = modem->payload_bytes_per_modem_frame;
        if (modem->freedv)
            frames_per_burst = freedv_get_frames_per_burst(modem->freedv);
        pthread_mutex_unlock(&modem_freedv_lock);

        size_t required = payload_bytes_per_modem_frame * (size_t)frames_per_burst;
        if (required == 0)
        {
            usleep(100000);
            continue;
        }

        if (data_size != required)
        {
            uint8_t *new_data = (uint8_t *)realloc(data, required);
            if (!new_data)
            {
                fprintf(stderr, "Failed to allocate memory for TX data.\n");
                usleep(100000);
                continue;
            }
            data = new_data;
            data_size = required;
        }

        if (size_buffer(data_tx_buffer_arq) >= required)
        {
            for (int i = 0; i < frames_per_burst; i++)
            {
                read_buffer(data_tx_buffer_arq, data + (payload_bytes_per_modem_frame * i), payload_bytes_per_modem_frame);
            }
            send_modulated_data(modem, data, frames_per_burst);
        }

        if (size_buffer(data_tx_buffer_broadcast) >= required)
        {
            for (int i = 0; i < frames_per_burst; i++)
            {
                read_buffer(data_tx_buffer_broadcast, data + (payload_bytes_per_modem_frame * i), payload_bytes_per_modem_frame);
            }
            send_modulated_data(modem, data, frames_per_burst);
        }

        if (size_buffer(data_tx_buffer_arq) < required &&
            size_buffer(data_tx_buffer_broadcast) < required)
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
    generic_modem_t *modem = (generic_modem_t *)g_modem;
    int last_pref_rx_mode = -1;
    int last_pref_tx_mode = -1;
    uint8_t *data = NULL;
    size_t data_size = 0;
    size_t nbytes_out = 0;

    while (!shutdown_)
    {
        int pref_rx_mode = arq_get_preferred_rx_mode();
        int pref_tx_mode = arq_get_preferred_tx_mode();
        if (pref_rx_mode != last_pref_rx_mode || pref_tx_mode != last_pref_tx_mode)
        {
            fprintf(stderr, "ARQ preferred modes: rx=%d tx=%d\n", pref_rx_mode, pref_tx_mode);
            last_pref_rx_mode = pref_rx_mode;
            last_pref_tx_mode = pref_tx_mode;
        }

        if (arq_conn.TRX != TX && pref_rx_mode >= 0)
            maybe_switch_modem_mode(modem, pref_rx_mode);

        uint32_t bitrate_bps = 0;
        size_t frame_bytes = 0;
        pthread_mutex_lock(&modem_freedv_lock);
        if (modem->freedv)
        {
            bitrate_bps = compute_bitrate_bps_locked(modem->freedv);
            uint32_t bits_per_modem_frame = (uint32_t)freedv_get_bits_per_modem_frame(modem->freedv);
            frame_bytes = bits_per_modem_frame / 8;
        }
        pthread_mutex_unlock(&modem_freedv_lock);

        if (frame_bytes == 0)
        {
            usleep(100000);
            continue;
        }
        if (data_size != frame_bytes)
        {
            uint8_t *new_data = (uint8_t *)realloc(data, frame_bytes);
            if (!new_data)
            {
                usleep(100000);
                continue;
            }
            data = new_data;
            data_size = frame_bytes;
        }

        // Always drain modem input, but ignore decoded frames while local TX is active.
        receive_modulated_data(modem, data, &nbytes_out);

        if (arq_conn.TRX == TX)
            continue;

        int sync = 0;
        float snr_est = 0.0f;
        int rx_status = 0;
        pthread_mutex_lock(&modem_freedv_lock);
        if (modem->freedv)
        {
            rx_status = freedv_get_rx_status(modem->freedv);
            freedv_get_modem_stats(modem->freedv, &sync, &snr_est);
        }
        pthread_mutex_unlock(&modem_freedv_lock);
        arq_update_link_metrics(sync, snr_est, rx_status, nbytes_out > 0);

        if (nbytes_out > 0)
        {
            size_t payload_nbytes = (nbytes_out >= 2) ? nbytes_out - 2 : 0;
            if (payload_nbytes == 0)
                continue;

            tnc_send_sn(snr_est);
            tnc_send_bitrate(0, bitrate_bps);

            int frame_type = parse_frame_header(data, payload_nbytes);

            switch (frame_type)
            {
            case PACKET_TYPE_ARQ_CONTROL:
            case PACKET_TYPE_ARQ_DATA:
                arq_handle_incoming_frame(data, payload_nbytes);
                break;
            case PACKET_TYPE_BROADCAST_CONTROL:
            case PACKET_TYPE_BROADCAST_DATA:
                write_buffer(data_rx_buffer_broadcast, data, payload_nbytes);
                break;
            default:
                printf("Unknown frame type received.\n");
                break;
            }
            
            printf("Received %zu payload bytes, packet type %d, frame_bytes: %zu\n", payload_nbytes, frame_type, frame_bytes);
        }
    }

    free(data);
    return NULL;
}

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

#include "freedv_api.h"
#include "fsk.h"
#include "ldpc_codes.h"
#include "ofdm_internal.h"

#include "defines.h"

int init_modem(struct freedv **freedv, int mode)
{
    char codename[80] = "H_256_512_4";
    struct freedv_advanced adv = {0, 2, 100, 8000, 1000, 200, codename};

    if (mode == FREEDV_MODE_FSK_LDPC)
        *freedv = freedv_open_advanced(mode, &adv);
    else 
        *freedv = freedv_open(mode);
    
    freedv_set_frames_per_burst(*freedv, 1);
    
    return 0;
}

int send_modulated_data(struct freedv *freedv, uint8_t *bytes_in, int frames_per_burst)
{
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    size_t payload_bytes_per_modem_frame =
        bytes_per_modem_frame - 2; /* 16 bits used for the CRC */
    size_t n_mod_out = freedv_get_n_tx_modem_samples(freedv);
    short mod_out_short[n_mod_out];

    /* send preamble */
    int n_preamble = freedv_rawdatapreambletx(freedv, mod_out_short);
    // TODO: write to buffer
    // fwrite(mod_out_short, sizeof(short), n_preamble, stdout);

    
    /* The raw data modes require a CRC in the last two bytes */
    uint16_t crc16 = freedv_gen_crc16(bytes_in, payload_bytes_per_modem_frame);
    bytes_in[bytes_per_modem_frame - 2] = crc16 >> 8;
    bytes_in[bytes_per_modem_frame - 1] = crc16 & 0xff;

    /* modulate and send a data frame */
    freedv_rawdatatx(freedv, mod_out_short, bytes_in);
    // TODO: write to buffer
    // fwrite(mod_out_short, sizeof(short), n_mod_out, stdout);

    /* send postamble */
    int n_postamble = freedv_rawdatapostambletx(freedv, mod_out_short);
    // TODO: write to buffer
    // fwrite(mod_out_short, sizeof(short), n_postamble, stdout);

    /* create some silence between bursts */
    int inter_burst_delay_ms = 200;
    int samples_delay = FREEDV_FS_8000 * inter_burst_delay_ms / 1000;
    short sil_short[samples_delay];
    for (int i = 0; i < samples_delay; i++) sil_short[i] = 0;
    // TODO: write to buffer
    // fwrite(sil_short, sizeof(short), samples_delay, stdout);
    
    return 0;
}

int receive_modulated_data(struct freedv *freedv, uint8_t *bytes_out, size_t *nbytes_out)
{
    int input_size = freedv_get_n_max_modem_samples(freedv);

    // TODO: read from buffer
    short demod_in[freedv_get_n_max_modem_samples(freedv)];

    size_t nin = freedv_nin(freedv);

    *nbytes_out = freedv_rawdatarx(freedv, bytes_out, demod_in);

    return 0;
}

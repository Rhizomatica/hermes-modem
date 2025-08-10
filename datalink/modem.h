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


#ifndef MODEM_H
#define MODEM_H

#include <stdint.h>
#include <stdbool.h>

#include "freedv_api.h"

int init_modem(struct freedv **freedv, int mode, int frames_per_burst, pthread_t *radio_capture, pthread_t *radio_playback);

int shutdown_modem(struct freedv *freedv, pthread_t *radio_capture, pthread_t *radio_playback);

// always send the frame size in bytes_in
int send_modulated_data(struct freedv *freedv, uint8_t *bytes_in, int frames_per_burst);

int receive_modulated_data(struct freedv *freedv, uint8_t *bytes_out, size_t *nbytes_out);


#endif // MODEM_H

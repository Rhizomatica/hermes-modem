#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "freedv_api.h"

int freedv_modes[] = { FREEDV_MODE_DATAC1,
                       FREEDV_MODE_DATAC3,
                       FREEDV_MODE_DATAC0,
                       FREEDV_MODE_DATAC4,
                       FREEDV_MODE_DATAC13,
                       FREEDV_MODE_DATAC14,
                       FREEDV_MODE_FSK_LDPC };

char *freedv_mode_names[] = { "DATAC1",
                              "DATAC3",
                              "DATAC0",
                              "DATAC4",
                              "DATAC13",
                              "DATAC14",
                              "FSK_LDPC" };

int main()
{
    printf("HERMES Modem Experimentation\n");
    
    struct freedv *freedv;

    for (int i = 0; i < sizeof(freedv_modes) / sizeof(freedv_modes[0]); i++)
    {
        printf("Opening mode %s (%d)\n", freedv_mode_names[i], freedv_modes[i]);

        freedv = freedv_open(freedv_modes[i]);
        if (freedv == NULL) {
            printf("Failed to open mode %d\n", freedv_modes[i]);
            return 1;
        }


        freedv_set_verbose(freedv, 2);


        size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
        size_t payload_bytes_per_modem_frame = bytes_per_modem_frame - 2; /* 16 bits used for the CRC */
        size_t n_mod_out = freedv_get_n_tx_modem_samples(freedv);
        // uint8_t bytes_in[bytes_per_modem_frame];
        // short mod_out_short[n_mod_out];

        printf("Modem frame size: %d bits\n", freedv_get_bits_per_modem_frame(freedv));
        printf("payload_bytes_per_modem_frame: %zu\n", payload_bytes_per_modem_frame);
        printf("n_mod_out: %zu\n", n_mod_out);
        
        if (freedv_modes[i] != FREEDV_MODE_FSK_LDPC) {
            // freedv_ofdm_print_info(freedv);
        }
        printf("\n");
      
        freedv_close(freedv);
    }
  
    return 0;

}

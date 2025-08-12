
#ifndef HAVE_DEFINES_MODEM_H
#define HAVE_DEFINES_MODEM_H

#define MAX_PATH 255

// {TX,RX}_SHM broadcast memory interface
#define SHM_PAYLOAD_BUFFER_SIZE 131072
#define SHM_PAYLOAD_NAME "/broadcast"

#define INT_BUFFER_SIZE 4096

#define DATA_TX_BUFFER_SIZE 8192
#define DATA_RX_BUFFER_SIZE 8192

// audio buffers shared memory interface
// 1536000 * 8
#define SIGNAL_BUFFER_SIZE 12288000
#define SIGNAL_INPUT "/signal-radio2modem"
#define SIGNAL_OUTPUT "/signal-modem2radio"

#if defined(_WIN32)
#define msleep(a) Sleep(a)
#else
#define msleep(a) usleep(a * 1000)
#endif


#endif // HAVE_DEFINES_MODEM_H

#ifndef ARQ_CHANNELS_H_
#define ARQ_CHANNELS_H_

#include <stddef.h>

#include "../third_party/chan/chan.h"
#include "arq_events.h"

#define ARQ_CH_CAP_TCP_CMD 64
#define ARQ_CH_CAP_TCP_PAYLOAD 128
#define ARQ_CH_CAP_MODEM_FRAME 128
#define ARQ_CH_CAP_MODEM_METRICS 128
#define ARQ_CH_CAP_MODEM_TX 128
#define ARQ_CH_CAP_TCP_STATUS 128
#define ARQ_CH_CAP_SHUTDOWN 1

typedef struct
{
    chan_t *tcp_cmd;
    chan_t *tcp_payload;
    chan_t *modem_frame;
    chan_t *modem_metrics;
    chan_t *modem_tx;
    chan_t *tcp_status;
    chan_t *shutdown;
} arq_channel_bus_t;

int arq_channel_bus_init(arq_channel_bus_t *bus);
void arq_channel_bus_close(arq_channel_bus_t *bus);
void arq_channel_bus_dispose(arq_channel_bus_t *bus);
int arq_channel_bus_try_send_cmd(arq_channel_bus_t *bus, const arq_cmd_msg_t *msg);
int arq_channel_bus_try_send_payload(arq_channel_bus_t *bus, const uint8_t *data, size_t len);
int arq_channel_bus_recv_cmd(arq_channel_bus_t *bus, arq_cmd_msg_t *msg);
int arq_channel_bus_recv_payload(arq_channel_bus_t *bus, arq_bytes_msg_t *msg);

#endif /* ARQ_CHANNELS_H_ */

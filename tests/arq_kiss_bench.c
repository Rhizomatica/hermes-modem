#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>

#include "../common/defines_modem.h"
#include "../common/ring_buffer_posix.h"
#include "../common/os_interop.h"
#include "../datalink_arq/arq.h"
#include "../datalink_arq/fsm.h"
#include "../datalink_broadcast/kiss.h"

// Globals expected by arq.c
bool shutdown_ = false;
cbuf_handle_t capture_buffer = NULL;
cbuf_handle_t playback_buffer = NULL;
cbuf_handle_t data_tx_buffer_arq = NULL;
cbuf_handle_t data_rx_buffer_arq = NULL;
cbuf_handle_t data_tx_buffer_broadcast = NULL;
cbuf_handle_t data_rx_buffer_broadcast = NULL;
static bool fsm_ready = false;

// --------------------------------------------------------------------
// Stub hooks required by linked objects (not used in this bench)
// --------------------------------------------------------------------
struct freedv;

int freedv_get_bits_per_modem_frame(struct freedv *unused)
{
    (void)unused;
    return (int)(arq_conn.frame_size * 8);
}

void tnc_send_connected(void) {}
void tnc_send_disconnected(void) {}
void ptt_on(void) {}
void ptt_off(void) {}

int shm_open_and_get_fd(const char *name, int oflag, mode_t mode)
{
    (void)name;
    (void)oflag;
    (void)mode;
    return -1;
}

int shm_create_and_get_fd(const char *name, int oflag, mode_t mode, size_t size)
{
    (void)name;
    (void)oflag;
    (void)mode;
    (void)size;
    return -1;
}

void *shm_map(int fd, size_t size, bool create)
{
    (void)fd;
    (void)size;
    (void)create;
    return NULL;
}

void shm_unmap(void *addr, size_t size)
{
    (void)addr;
    (void)size;
}

static void init_buffers(void)
{
    uint8_t *tx_mem = calloc(1, DATA_TX_BUFFER_SIZE);
    uint8_t *rx_mem = calloc(1, DATA_RX_BUFFER_SIZE);
    uint8_t *btx_mem = calloc(1, DATA_TX_BUFFER_SIZE);
    uint8_t *brx_mem = calloc(1, DATA_RX_BUFFER_SIZE);

    data_tx_buffer_arq = circular_buf_init(tx_mem, DATA_TX_BUFFER_SIZE);
    data_rx_buffer_arq = circular_buf_init(rx_mem, DATA_RX_BUFFER_SIZE);
    data_tx_buffer_broadcast = circular_buf_init(btx_mem, DATA_TX_BUFFER_SIZE);
    data_rx_buffer_broadcast = circular_buf_init(brx_mem, DATA_RX_BUFFER_SIZE);
}

static void free_buffers(void)
{
    if (data_tx_buffer_arq)
    {
        free(data_tx_buffer_arq->buffer);
        circular_buf_free(data_tx_buffer_arq);
    }
    if (data_rx_buffer_arq)
    {
        free(data_rx_buffer_arq->buffer);
        circular_buf_free(data_rx_buffer_arq);
    }
    if (data_tx_buffer_broadcast)
    {
        free(data_tx_buffer_broadcast->buffer);
        circular_buf_free(data_tx_buffer_broadcast);
    }
    if (data_rx_buffer_broadcast)
    {
        free(data_rx_buffer_broadcast->buffer);
        circular_buf_free(data_rx_buffer_broadcast);
    }
}

static void reset_arq_state(void)
{
    clear_buffer(data_tx_buffer_arq);
    clear_buffer(data_rx_buffer_arq);
    reset_arq_info(&arq_conn);
    arq_conn.frame_size = 64; // keep tests lightweight
    arq_conn.payload_size = arq_conn.frame_size - HEADER_SIZE;
    arq_conn.call_burst_size = 2;
    snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "PU2UIT");
    snprintf(arq_conn.dst_addr, CALLSIGN_MAX_SIZE, "PU2GNU");
    arq_conn.listen = false;
    arq_conn.modem = NULL;
    init_model();
}

static bool establish_link(void)
{
    uint8_t frame[INT_BUFFER_SIZE];

    if (fsm_ready)
        fsm_destroy(&arq_fsm);
    fsm_init(&arq_fsm, state_no_connected_client);
    fsm_ready = true;

    fsm_dispatch(&arq_fsm, EV_CLIENT_CONNECT);
    snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "PU2UIT");
    snprintf(arq_conn.dst_addr, CALLSIGN_MAX_SIZE, "PU2GNU");
    fsm_dispatch(&arq_fsm, EV_LINK_CALL_REMOTE);

    size_t expected = arq_conn.frame_size;
    if (size_buffer(data_tx_buffer_arq) < expected)
    {
        fprintf(stderr, "[bench] insufficient call frames queued\n");
        return false;
    }
    read_buffer(data_tx_buffer_arq, frame, expected);

    uint8_t packet_type = 0, subtype = 0, sequence = 0;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (parse_arq_frame(frame, expected, &packet_type, &subtype, &sequence, &payload, &payload_len) < 0)
    {
        fprintf(stderr, "[bench] failed to parse call request frame\n");
        return false;
    }
    if (packet_type != PACKET_ARQ_CONTROL || subtype != CONTROL_CALL_REQUEST)
    {
        fprintf(stderr, "[bench] unexpected frame type in call request\n");
        return false;
    }

    uint8_t response[INT_BUFFER_SIZE];
    int resp_len = create_control_frame(response, sizeof(response), CONTROL_CALL_RESPONSE, arq_conn.dst_addr);
    if (resp_len <= 0)
    {
        fprintf(stderr, "[bench] failed to craft call response\n");
        return false;
    }

    if (process_arq_frame(response, (size_t)resp_len) < 0)
    {
        fprintf(stderr, "[bench] process_arq_frame rejected response\n");
        return false;
    }

    if (arq_fsm.current != state_link_connected)
    {
        fprintf(stderr, "[bench] FSM did not enter link_connected\n");
        return false;
    }

    clear_buffer(data_tx_buffer_arq); // drop burst copies before next test
    return true;
}

static bool test_arq_data_flow(void)
{
    if (!establish_link())
        return false;

    clear_buffer(data_tx_buffer_arq);
    clear_buffer(data_rx_buffer_arq);
    arq_conn.rx_sequence = 0;

    const uint8_t payload[] = "HELLO_WO";
    uint8_t frame[INT_BUFFER_SIZE];
    int frame_len = create_data_frame(frame, arq_conn.frame_size, (uint8_t *)payload, sizeof(payload), arq_conn.rx_sequence);
    if (frame_len <= 0)
    {
        fprintf(stderr, "[bench] failed to create data frame\n");
        return false;
    }

    if (process_arq_frame(frame, (size_t)frame_len) < 0)
    {
        fprintf(stderr, "[bench] process_arq_frame rejected data frame\n");
        return false;
    }

    uint8_t ack[INT_BUFFER_SIZE];
    size_t ack_len = read_buffer_all(data_tx_buffer_arq, ack);
    if (ack_len == 0)
    {
        fprintf(stderr, "[bench] no ACK emitted\n");
        return false;
    }

    uint8_t packet_type = 0, subtype = 0, sequence = 0;
    uint8_t *payload_ptr = NULL;
    size_t payload_len = 0;
    if (parse_arq_frame(ack, ack_len, &packet_type, &subtype, &sequence, &payload_ptr, &payload_len) < 0 ||
        packet_type != PACKET_ARQ_ACK)
    {
        fprintf(stderr, "[bench] ACK parsing failed\n");
        return false;
    }

    size_t delivered = size_buffer(data_rx_buffer_arq);
    if (delivered != sizeof(payload))
    {
        fprintf(stderr, "[bench] expected %zu bytes delivered, got %zu\n", sizeof(payload), delivered);
        return false;
    }
    uint8_t delivered_payload[INT_BUFFER_SIZE];
    read_buffer(data_rx_buffer_arq, delivered_payload, delivered);
    if (memcmp(delivered_payload, payload, delivered) != 0)
    {
        fprintf(stderr, "[bench] payload mismatch\n");
        return false;
    }

    return true;
}

static bool test_kiss_roundtrip(void)
{
    const uint8_t message[] = "KISS_PAYLOAD";
    uint8_t encoded[MAX_PAYLOAD * 2];
    uint8_t decoded[MAX_PAYLOAD];

    int encoded_len = kiss_write_frame((uint8_t *)message, (int)sizeof(message), encoded);
    if (encoded_len <= 0)
    {
        fprintf(stderr, "[bench] kiss_write_frame failed\n");
        return false;
    }

    uint8_t dummy[MAX_PAYLOAD];
    kiss_read(FEND, dummy); // reset internal state

    int result = 0;
    for (int i = 0; i < encoded_len; i++)
    {
        result = kiss_read(encoded[i], decoded);
        if (result > 0)
            break;
    }

    if (result != (int)sizeof(message))
    {
        fprintf(stderr, "[bench] kiss_read returned %d, expected %zu\n", result, sizeof(message));
        return false;
    }
    if (memcmp(decoded, message, sizeof(message)) != 0)
    {
        fprintf(stderr, "[bench] KISS payload mismatch\n");
        return false;
    }

    return true;
}

int main(void)
{
    init_buffers();
    reset_arq_state();

    bool arq_ok = establish_link();
    bool data_ok = arq_ok && test_arq_data_flow();
    bool kiss_ok = test_kiss_roundtrip();

    free_buffers();
    if (fsm_ready)
        fsm_destroy(&arq_fsm);

    printf("ARQ call flow: %s\n", arq_ok ? "PASS" : "FAIL");
    printf("ARQ data flow: %s\n", data_ok ? "PASS" : "FAIL");
    printf("KISS framing: %s\n", kiss_ok ? "PASS" : "FAIL");

    return (arq_ok && data_ok && kiss_ok) ? EXIT_SUCCESS : EXIT_FAILURE;
}

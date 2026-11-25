#include "cn_proto.h"

#include <string.h>

size_t cn_encode(uint8_t *buf, size_t cap, uint8_t type,
                 const uint8_t src[CN_MAC_LEN], uint16_t seq, uint32_t lamport,
                 const uint8_t *payload, uint8_t len)
{
    if (buf == NULL || src == NULL || cap < (size_t)CN_HDR_LEN + len) {
        return 0;
    }
    if (len > 0 && payload == NULL) {
        return 0;
    }
    buf[0] = type;
    memcpy(&buf[1], src, CN_MAC_LEN);
    buf[7]  = (uint8_t)(seq & 0xFF);
    buf[8]  = (uint8_t)(seq >> 8);
    buf[9]  = (uint8_t)(lamport & 0xFF);
    buf[10] = (uint8_t)((lamport >> 8) & 0xFF);
    buf[11] = (uint8_t)((lamport >> 16) & 0xFF);
    buf[12] = (uint8_t)((lamport >> 24) & 0xFF);
    buf[13] = len;
    if (len > 0) {
        memcpy(&buf[CN_HDR_LEN], payload, len);
    }
    return (size_t)CN_HDR_LEN + len;
}

bool cn_decode(const uint8_t *buf, size_t n, cn_header_t *out)
{
    if (buf == NULL || out == NULL || n < CN_HDR_LEN) {
        return false;
    }
    if (buf[0] < CN_MSG_STATE || buf[0] > CN_MSG_TASK_RESULT) {
        return false;
    }
    if ((size_t)buf[13] != n - CN_HDR_LEN) {
        return false;
    }
    out->type = buf[0];
    memcpy(out->src, &buf[1], CN_MAC_LEN);
    out->seq = (uint16_t)(buf[7] | (buf[8] << 8));
    out->lamport = (uint32_t)buf[9] | ((uint32_t)buf[10] << 8) |
                   ((uint32_t)buf[11] << 16) | ((uint32_t)buf[12] << 24);
    out->len = buf[13];
    out->payload = &buf[CN_HDR_LEN];
    return true;
}

uint8_t cn_q8(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

float cn_unq8(uint8_t q)
{
    return (float)q / 255.0f;
}

const char *cn_event_name(cn_event_type_t t)
{
    static const char *const names[CN_EV_COUNT] = {
        "NODE_START", "NEIGHBOR_UP", "NEIGHBOR_DOWN", "TASK_SUBMITTED",
        "TASK_ASSIGNED", "TASK_ACCEPTED", "TASK_REJECTED", "TASK_STARTED",
        "TASK_DONE", "TASK_TIMEOUT", "TASK_REASSIGNED", "TASK_FAILED"
    };
    if ((int)t < 0 || t >= CN_EV_COUNT) return "UNKNOWN";
    return names[t];
}

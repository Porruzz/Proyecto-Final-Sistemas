/* cn_proto.h - formato binario de los mensajes de ComplexNet.
 *
 * Todos los campos multibyte van en little-endian y se serializan byte a
 * byte (sin castear structs), por lo que el formato no depende del
 * alineamiento ni del compilador.
 *
 *   off  tam  campo
 *    0    1   type      cn_msg_type_t
 *    1    6   src       MAC del emisor
 *    7    2   seq       contador de mensajes del emisor
 *    9    4   lamport   reloj logico de Lamport del emisor
 *   13    1   len       bytes de payload
 *   14   len  payload
 *
 * Payloads:
 *   STATE        C(1) M(1) L(1)                          (q8: valor*255)
 *   TASK_ASSIGN  id(2) kind(1) cpu(1) mem(1) attempt(1) dlen(1) data[dlen]
 *   TASK_ACK     id(2) accepted(1)
 *   TASK_RESULT  id(2) status(1) dlen(1) data[dlen]
 */
#ifndef CN_PROTO_H
#define CN_PROTO_H

#include "cn_types.h"

#define CN_HDR_LEN 14

typedef struct {
    uint8_t        type;
    uint8_t        src[CN_MAC_LEN];
    uint16_t       seq;
    uint32_t       lamport;
    uint8_t        len;
    const uint8_t *payload;   /* apunta dentro del buffer original */
} cn_header_t;

/* Devuelve el tamano total del frame, o 0 si no cabe en cap. */
size_t cn_encode(uint8_t *buf, size_t cap, uint8_t type,
                 const uint8_t src[CN_MAC_LEN], uint16_t seq, uint32_t lamport,
                 const uint8_t *payload, uint8_t len);

/* Valida y decodifica. false si el frame es corto, inconsistente o de tipo
 * desconocido. */
bool cn_decode(const uint8_t *buf, size_t n, cn_header_t *out);

uint8_t cn_q8(float v);       /* [0,1] -> [0,255], saturando */
float   cn_unq8(uint8_t q);

#endif /* CN_PROTO_H */

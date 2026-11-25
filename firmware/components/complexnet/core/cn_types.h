/* cn_types.h - tipos y constantes compartidos del nucleo de ComplexNet API.
 *
 * El nucleo (carpeta core/) NO depende de ESP-IDF: se compila igual en el
 * ESP32 y en un PC con gcc, lo que permite probar la logica distribuida
 * (descubrimiento, scheduler, fallos, reasignacion) sin hardware.
 */
#ifndef CN_TYPES_H
#define CN_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CN_MAC_LEN              6
#define CN_MAX_NEIGHBORS        8      /* vecinos que un nodo puede recordar   */
#define CN_MAX_TASKS            8      /* tareas propias en vuelo (origen)     */
#define CN_MAX_RUNNING          2      /* tareas ejecutandose a la vez (nodo)  */
#define CN_MAX_DATA             160    /* bytes de datos/resultado por tarea   */
#define CN_FRAME_MAX            250    /* limite de payload de ESP-NOW         */

#define CN_HEARTBEAT_MS         2000u  /* periodo de difusion del estado       */
#define CN_NEIGHBOR_TIMEOUT_MS  6000u  /* 3 heartbeats perdidos => nodo caido  */
#define CN_ACK_TIMEOUT_MS       1500u  /* espera del ACK de una asignacion     */
#define CN_TASK_TIMEOUT_MS      30000u /* plazo por defecto de una tarea       */
#define CN_MAX_ATTEMPTS         3      /* intentos de asignacion por tarea     */
#define CN_RETAIN_MS            5000u  /* tiempo que se conserva una tarea     */
                                       /* terminada antes de liberar su slot   */

/* Vector de estado Si(t) = [C, M, B, R, A] del documento de Fase 1.
 * A (disponibilidad) no se almacena: es "active" en la tabla de vecinos.
 * L es la carga actual (tareas en ejecucion / CN_MAX_RUNNING).               */
typedef struct {
    float C;   /* CPU libre            [0,1]                                   */
    float M;   /* memoria libre        [0,1]                                   */
    float B;   /* calidad del enlace   [0,1], estimada con el RSSI observado   */
    float R;   /* reputacion           [0,1], calculada por el observador      */
    float L;   /* carga                [0,1]                                   */
} cn_state_t;

/* Pesos de F(i,j) = w1*C + w2*M + w3*B + w4*R - w5*L                         */
typedef struct { float w1, w2, w3, w4, w5; } cn_weights_t;

typedef enum {
    CN_MSG_STATE       = 1,   /* difusion: estado del nodo (heartbeat)        */
    CN_MSG_TASK_ASSIGN = 2,   /* unicast: origen -> ejecutor                  */
    CN_MSG_TASK_ACK    = 3,   /* unicast: ejecutor -> origen                  */
    CN_MSG_TASK_RESULT = 4    /* unicast: ejecutor -> origen                  */
} cn_msg_type_t;

typedef enum {
    CN_TASK_FREE = 0,
    CN_TASK_PENDING_ACK,      /* ASSIGN enviado, esperando ACK                */
    CN_TASK_RUNNING,          /* el ejecutor acepto, esperando resultado      */
    CN_TASK_DONE,
    CN_TASK_FAILED
} cn_task_status_t;

typedef enum {
    CN_EV_NODE_START = 0,
    CN_EV_NEIGHBOR_UP,
    CN_EV_NEIGHBOR_DOWN,
    CN_EV_TASK_SUBMITTED,
    CN_EV_TASK_ASSIGNED,
    CN_EV_TASK_ACCEPTED,
    CN_EV_TASK_REJECTED,
    CN_EV_TASK_STARTED,
    CN_EV_TASK_DONE,
    CN_EV_TASK_TIMEOUT,
    CN_EV_TASK_REASSIGNED,
    CN_EV_TASK_FAILED,
    CN_EV_COUNT
} cn_event_type_t;

/* Codigos de retorno de la API */
#define CN_OK                 0
#define CN_ERR_ARG           (-1)
#define CN_ERR_FULL          (-2)   /* tabla de tareas llena                  */
#define CN_ERR_NO_NODE       (-3)   /* ningun nodo cumple los requisitos      */
#define CN_ERR_NOT_FOUND     (-4)
#define CN_ERR_MAX_ATTEMPTS  (-5)

/* Estados reportados al usuario en on_result */
#define CN_STATUS_OK            0
#define CN_STATUS_ERROR         1   /* el manejador de la tarea fallo         */
#define CN_STATUS_GAVE_UP     100   /* el origen agoto intentos / sin nodos   */

const char *cn_event_name(cn_event_type_t t);

#endif /* CN_TYPES_H */

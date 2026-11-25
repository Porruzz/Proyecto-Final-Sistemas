# Simulador de ComplexNet API

Ejecuta el MISMO codigo del nucleo (`firmware/components/complexnet/core`) que corre en el ESP32,
pero en el PC. Solo se reemplaza la radio por una red simulada.

## Requisitos
- gcc y make (Linux/macOS: ya suelen estar; Windows: WSL con Ubuntu, o MSYS2 con `pacman -S gcc make`)

## Uso (desde la carpeta tests/host)
    make test         # las 41 pruebas en C (unitarias + integracion)
    make sanitize     # las mismas con AddressSanitizer + UBSan
    make demo         # una falla de ejecutor, con el registro JSON del nodo origen
    make escenario    # compila el simulador interactivo
    ./build/escenario -h

## Escenarios (./build/escenario)
    -n N      nodos (2..6)            -t K     tareas desde el nodo 0
    -d SEG    duracion de cada tarea  -T SEG   duracion total
    -l PCT    perdida de paquetes     -u PCT   paquetes duplicados
    -s SEM    semilla aleatoria       -q       solo resumen
    -k I@SEG  mata el nodo I en el segundo SEG   (repetible)
    -r I@SEG  revive el nodo I en el segundo SEG (repetible)
    -c A-B    corta el enlace A-B                (repetible)

Nodos numerados desde 0. Las tareas se envian en t = 5 s (tras el descubrimiento).
El nodo de mayor indice tiene mas CPU/memoria libres, asi que suele ser el elegido.

Ejemplos:
    ./build/escenario                          # caso base: 3 nodos, 1 tarea
    ./build/escenario -k 2@8                   # cae el ejecutor a mitad de tarea
    ./build/escenario -k 2@8 -k 1@9 -q         # caen dos nodos: la tarea acaba en el origen
    ./build/escenario -n 4 -t 4 -l 30 -T 120   # 30 % de perdida, 4 tareas
    ./build/escenario -u 100                   # todos los paquetes duplicados
    ./build/escenario -k 2@8 -r 2@20           # el nodo cae y se reincorpora
    ./build/escenario -c 0-2                   # sin enlace directo entre 0 y 2

## Archivos
    sim_net.c/.h          simulador (reloj virtual 10 ms, latencia 5-10 ms, perdida, duplicados, caidas)
    escenario.c           ejecutor de escenarios por linea de comandos
    demo_failure.c        ejemplo minimo de uso de la API del simulador
    test_proto.c          pruebas de protocolo y planificador
    test_core.c           pruebas del nucleo con red simulada
    test_integration.c    pruebas de integracion (fallas, perdida, contencion, sobrecarga...)
    test_util.h           mini framework de pruebas (CHECK, RUN)

## Escribir un escenario propio en C
    #include "sim_net.h"
    static sim_t S;
    int main(void){
        sim_init(&S, 3, 42);              // 3 nodos, semilla 42
        S.n[2].C = 0.9f; S.n[2].M = 0.8f; // recursos "reales" del nodo 2
        S.loss_pct = 10;                  // 10 % de perdida
        sim_run(&S, 5000);                // 5 s de descubrimiento
        uint8_t d = 50; uint16_t id;
        sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 30000, &id);  // tarea de 5 s
        sim_kill(&S, 2);                  // cae el nodo 2
        sim_run(&S, 20000);
        const sim_result_t *r = sim_find_result(&S, 0, id);      // resultado en el origen
        return r && r->status == CN_STATUS_OK ? 0 : 1;
    }
Compilar con la misma linea que usa el Makefile (ver regla de demo_failure).

## Alcance
Es una SIMULACION: la perdida de paquetes es uniforme e independiente, no hay interferencia ni
rafagas, y el tiempo es virtual. Valida la logica del nucleo, no el comportamiento de la radio ESP-NOW.

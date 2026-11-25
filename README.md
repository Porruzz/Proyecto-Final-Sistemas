# ComplexNet API — Fase 3 (simulación)

Universidad Sergio Arboleda · Sistemas Complejos · PCIA5016

## Inicio rápido
Requisitos: Python 3.10+ y GCC disponible en PATH. Windows: instalar MSYS2 UCRT64 GCC y agregar `C:\msys64\ucrt64\bin` al PATH. Ubuntu: `sudo apt install build-essential python3`.
Desde la raíz: `python dashboard/app.py` (o `python3 dashboard/app.py`). Abrir http://127.0.0.1:8000. No requiere pip. En Windows usar iniciar_simulador.bat.

## Demostración
1. Reiniciar con tres nodos, Fitness, pérdida 0%. Observar vecinos y recursos.
2. Ejecutar tarea y escenario de fallo (este último reinicia la red).
3. Reiniciar para recuperar los tres nodos. Cargar una imagen y convertir a gris.
4. Entrenar regresión con los datos de ejemplo. Resultado esperado: pendiente 2, intercepto 3.
5. Cambiar estrategia, número de nodos y pérdida. Avanzar reloj virtual manualmente.

## Pruebas
`make -C tests/host test` (Linux o MSYS2 con make). `python experiments/benchmark.py` genera CSV/JSON y verifica aplicaciones y recuperación. Los resultados incluidos fueron obtenidos en Linux.

## Arquitectura
Dashboard HTTP local → API Python ctypes → bridge C → simulador de radio → instancias independientes del núcleo C. El servidor web no decide el ejecutor: cada origen usa su tabla de vecinos. Los manejadores de imágenes y regresión se ejecutan en el puerto simulado C. No hay comunicación física en esta entrega.

## Límites
3–6 nodos; 160 bytes por tarea; 2 ejecuciones simultáneas por nodo. Las aplicaciones envían fragmentos secuencialmente, no prueban aceleración paralela. El dashboard reduce imágenes a máximo 32×32. Los recursos son sintéticos; latencias virtuales. El historial es acotado y se limpia por tarea; los eventos mostrados son recientes. La topología visual representa enlaces potenciales entre nodos activos, no mediciones de radio. El benchmark usa tareas secuenciales de 500 ms virtuales y no representa saturación. La política Round Robin recorre candidatos elegibles en orden local; tras reinicio de nodo vuelve la política por defecto. El protocolo de regresión usa float32 y double little-endian en hosts compatibles. No exponer el servidor fuera de localhost.

## Hardware pendiente
El ZIP de Fase 2 no incluye puerto ESP-NOW, cliente serie ni configuración ESP-IDF. Se requiere reconstruirlos, integrar los manejadores 3 y 4, compilar y probar tres placas. No se afirma cumplimiento físico.

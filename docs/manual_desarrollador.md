# Manual del desarrollador
Desde la raíz del proyecto:
```python
from simulator.api import reset, Node, snapshot
from applications.apps import regression, image
reset(n=3, seed=1, loss=0, strategy=0)
print(Node(0).run(1, bytes([10])))
print(regression([(1,5),(2,7),(3,9)]))
print(snapshot())
```
`reset`: 3–6 nodos, pérdida 0–50%, estrategias 0 Fitness, 1 menor carga, 2 Round Robin, 3 aleatoria. `Node.submit(kind,data)` devuelve identificador y tiempo inicial; `result(handle)` espera avanzando el reloj virtual; `run` ejecuta una tarea y libera historial anterior. No mezclar `run` con tareas pendientes. Los resultados incluyen status (0 éxito), executor (índice desde cero), latency_ms y data (bytes). `snapshot` devuelve recursos y eventos recientes. `image` acepta RGB empaquetado; `regression` acepta pares numéricos. Usar un solo proceso Python por simulación.

Tipos: 1 espera (primer byte ×100 ms), 2 eco de ID y ejecutor, 3 RGB a gris (máximo 159 bytes múltiplo de 3), 4 estadísticas de regresión (hasta 20 pares float32 little-endian; salida cinco double). El origen combina estadísticas; los ejecutores calculan cada fragmento. Las tareas son idempotentes.

Para hardware, implementar el contrato cn_port_t en ESP-IDF y una API serie equivalente a PING/SNAPSHOT/SUBMIT/SETRES del manual de Fase 2. El transporte físico no está implementado en esta versión.

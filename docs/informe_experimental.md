# Informe experimental preliminar — Fase 3
## Objetivo y método
Evaluar la asignación local de tareas sobre el núcleo portable C de ComplexNet. Se ejecutaron 192 escenarios factoriales: 4 tamaños (3–6 nodos), 4 políticas, 4 pérdidas (0,10,20,30%) y 3 semillas. Cada escenario envía 12 tareas de 500 ms virtuales, secuencialmente, alternando origen. Se espera 5100 ms entre tareas para liberar registros retenidos. Los recursos iniciales son sintéticos y heterogéneos.
## Resultados
Fitness 572/576 (99.31%), menor carga 569/576 (98.78%), Round Robin 569/576 (98.78%), aleatoria 564/576 (97.92%). La regresión sobre 60 puntos y=2x+3 devuelve pendiente 2, intercepto 3, MSE 0. La conversión de 60 píxeles rojos produce 76 por píxel. En un escenario de caída del ejecutor la tarea termina en 15690 ms virtuales. Las 41 pruebas originales pasan (383 verificaciones).
## Interpretación
Las decisiones locales basadas en recursos y reputación ilustran autoorganización y adaptación. La recuperación ante pérdida del ejecutor evidencia robustez bajo el escenario probado. No implica tolerancia a pérdida del origen. La diferencia de éxito entre estrategias es descriptiva; no se ha realizado inferencia estadística. El benchmark no mide aceleración paralela ni carga simultánea. Los CSV permiten ampliar el análisis.
## Pendientes
Medir latencia de comunicación separada de duración de tarea, tiempo de asignación, descubrimiento, ancho de banda real, CPU y memoria físicas, disponibilidad temporal, concurrencia y saturación. Validar tres ESP32. Los recursos y tiempos simulados no son mediciones físicas. El informe es preliminar y no sustituye la entrega final.

# ComplexNet — Modo Hardware Real (2+ ESP-WROOM-32 con ESP-NOW)

## Archivos principales
- `firmware/arduino/complexnet_esp32/complexnet_esp32.ino`: sketch listo para Arduino IDE (`ESP32 Dev Module`), usando el mismo núcleo C portable (`src/cn_core.c`, `cn_proto.c`, `cn_scheduler.c`) sobre radio física **ESP-NOW** (canal 1, sin router).
- `hardware/dashboard_esp32.py` + `hardware/serial_win.py` + `hardware/esp32_client.py`: puente USB serie sin dependencias externas en Windows y dashboard web local (`http://127.0.0.1:8001`).
- `iniciar_hardware_esp32.bat`: acceso directo para iniciar el dashboard en Windows.

## Uso rápido en cualquier computador Windows
1. Tener instalado **Python 3.10+** (no requiere `pip install` ni `gcc` en Windows; en Linux/macOS ejecutar `pip install pyserial`).
2. Tener instalado el driver USB-Serial **CP210x** (Silicon Labs) o **CH340** según la placa ESP32.
3. Si las placas aún no están programadas, abrir `firmware/arduino/complexnet_esp32/complexnet_esp32.ino` en Arduino IDE, elegir `ESP32 Dev Module` y subir exactamente el mismo firmware a ambos ESP-WROOM-32.
4. Conectar el **ESP32 #1** al PC con el cable USB de datos y energizar el **ESP32 #2** (otro puerto USB o cargador 5 V).
5. Cerrar el Monitor Serie de Arduino IDE, ejecutar `iniciar_hardware_esp32.bat`, abrir `http://127.0.0.1:8001` y pulsar **Auto-detectar ESP32**.

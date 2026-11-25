"""
dashboard_esp32.py - Dashboard web local para 2+ ESP-WROOM-32 físicos (ESP-NOW).

Ejecutar desde la raíz del proyecto:
    python hardware/dashboard_esp32.py
Abrir:
    http://127.0.0.1:8001
"""

from __future__ import annotations

import base64
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from hardware.esp32_client import ESP32ComplexNetClient
from hardware.serial_win import list_serial_ports

client = ESP32ComplexNetClient()
HTML_PATH = Path(__file__).with_name("index.html")


class HardwareHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        # Mantener la consola limpia
        return

    def _reply(self, obj, status: int = 200) -> None:
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/":
            html = HTML_PATH.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
            return

        if self.path == "/ports":
            self._reply({"ports": list_serial_ports(), "connected": client.connected, "port": client.port_name})
            return

        if self.path == "/state":
            if not client.connected:
                self._reply({"connected": False, "ports": list_serial_ports()})
                return
            try:
                snap = client.snapshot()
                snap["connected"] = True
                self._reply(snap)
            except Exception as exc:
                self._reply({"connected": client.connected, "error": str(exc)}, 500)
            return

        self._reply({"error": "Ruta no encontrada"}, 404)

    def do_POST(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            d = json.loads(self.rfile.read(length) or b"{}")
            action = d.get("action")

            if action == "connect":
                port = d.get("port")
                res = client.connect(port) if port else client.auto_connect()
                self._reply(res)
                return

            if action == "disconnect":
                client.disconnect()
                self._reply({"connected": False})
                return

            if action == "strategy":
                self._reply(client.set_strategy(int(d.get("strategy", 0))))
                return

            if action == "setres":
                if d.get("auto"):
                    self._reply(client.set_resources(None, None))
                else:
                    self._reply(client.set_resources(float(d["c"]), float(d["m"])))
                return

            if action == "drop":
                self._reply(client.set_drop(int(d.get("pct", 0))))
                return

            if action == "reset":
                self._reply(client.reset())
                return

            if action == "sleep":
                dur_ds = max(1, min(250, int(d.get("duration_ds", 10))))
                res = client.run_task(1, bytes([dur_ds]), wait_timeout=max(20.0, dur_ds * 0.25 + 15.0))
                res.pop("data", None)
                self._reply(res)
                return

            if action == "echo":
                res = client.run_task(2, b"", wait_timeout=10.0)
                res.pop("data", None)
                self._reply(res)
                return

            if action == "failure_start":
                dur_ds = int(d.get("duration_ds", 100))
                self._reply(client.start_failure_demo(dur_ds))
                return

            if action == "image":
                rgb = base64.b64decode(d["rgb"])
                res = client.run_image_gray(rgb)
                self._reply({
                    "gray": base64.b64encode(res["gray"]).decode("ascii"),
                    "workers": res["workers"],
                    "fragments": res["fragments"],
                    "total_ms": res["total_ms"],
                    "avg_fragment_ms": res["avg_fragment_ms"],
                })
                return

            if action == "ai":
                self._reply(client.run_regression(d["points"]))
                return

            self._reply({"error": f"Acción desconocida: {action}"}, 400)
        except Exception as exc:
            self._reply({"error": str(exc)}, 400)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        try:
            info = client.connect(sys.argv[1])
            print(f"Conectado automáticamente a {info['port']} (MAC {info.get('mac')})")
        except Exception as exc:
            print(f"Aviso al conectar {sys.argv[1]}: {exc}")
    print("ComplexNet Hardware (ESP-WROOM-32): http://127.0.0.1:8001")
    print("Presiona Ctrl+C para salir.")
    ThreadingHTTPServer(("127.0.0.1", 8001), HardwareHandler).serve_forever()

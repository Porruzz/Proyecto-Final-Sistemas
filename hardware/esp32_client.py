"""
esp32_client.py - Cliente serie de ComplexNet para ESP-WROOM-32 real por USB.

Se conecta a 1 ESP32 conectado por el cable USB de datos y envía tareas/consultas
que ese ESP32 distribuye por radio real ESP-NOW hacia sus vecinos físicos (el 2.º ESP32).
"""

from __future__ import annotations

import json
import struct
import threading
import time
from typing import Any, Dict, List, Optional, Tuple

from hardware.serial_win import SerialPort, list_serial_ports


class ESP32ComplexNetClient:
    def __init__(self) -> None:
        self._ser: Optional[SerialPort] = None
        self._port_name: Optional[str] = None
        self._reader_thread: Optional[threading.Thread] = None
        self._stop_evt = threading.Event()
        self._cmd_lock = threading.Lock()
        self._write_lock = threading.Lock()
        self._cond = threading.Condition()

        self._last_by_type: Dict[str, Tuple[int, Dict[str, Any]]] = {}
        self._seq = 0
        self._results: Dict[int, Dict[str, Any]] = {}
        self._last_error: Optional[str] = None
        self._failure_job: Optional[Dict[str, Any]] = None

    @property
    def connected(self) -> bool:
        return self._ser is not None and not self._stop_evt.is_set()

    @property
    def port_name(self) -> Optional[str]:
        return self._port_name

    def connect(self, port: str, baudrate: int = 115200) -> Dict[str, Any]:
        self.disconnect()
        self._stop_evt.clear()
        self._last_error = None
        self._results.clear()
        self._last_by_type.clear()

        ser = SerialPort(port, baudrate=baudrate, timeout=0.1)
        self._ser = ser
        self._port_name = port
        self._reader_thread = threading.Thread(
            target=self._read_loop, name=f"esp32-rx-{port}", daemon=True
        )
        self._reader_thread.start()

        # Pequeña pausa por si la placa apenas terminó de arrancar y verificar con PING
        try:
            pong = self.ping(timeout=2.5)
            return {"connected": True, "port": port, **pong}
        except Exception:
            # Reintento breve (algunos chips CP210x tardan ~0.5 s tras abrir el puerto)
            time.sleep(0.5)
            try:
                pong = self.ping(timeout=2.5)
                return {"connected": True, "port": port, **pong}
            except Exception as exc:
                self.disconnect()
                raise RuntimeError(
                    f"El puerto {port} abrió, pero el ESP32 no respondió PING. "
                    f"¿Ya subiste complexnet_esp32.ino? ({exc})"
                ) from exc

    def auto_connect(self) -> Dict[str, Any]:
        ports = list_serial_ports()
        if not ports:
            raise RuntimeError(
                "No se detectó ningún puerto COM. Conecta un ESP32 con el cable de datos "
                "y verifica el driver CP210x / CH340."
            )
        errors = []
        for entry in ports:
            p = entry["port"]
            try:
                return self.connect(p)
            except Exception as exc:
                errors.append(f"{p}: {exc}")
        raise RuntimeError("No se encontró ningún ESP32 con ComplexNet:\n" + "\n".join(errors))

    def disconnect(self) -> None:
        self._stop_evt.set()
        ser = self._ser
        self._ser = None
        self._port_name = None
        if ser is not None:
            ser.close()
        if self._reader_thread is not None and self._reader_thread.is_alive():
            self._reader_thread.join(timeout=1.0)
        self._reader_thread = None
        with self._cond:
            self._cond.notify_all()

    def _read_loop(self) -> None:
        buf = bytearray()
        while not self._stop_evt.is_set():
            ser = self._ser
            if ser is None:
                break
            try:
                chunk = ser.read(1024)
            except Exception as exc:
                if not self._stop_evt.is_set():
                    self._last_error = str(exc)
                    self._stop_evt.set()
                    with self._cond:
                        self._cond.notify_all()
                break
            if not chunk:
                continue
            buf.extend(chunk)
            while b"\n" in buf:
                line_bytes, _, rest = buf.partition(b"\n")
                buf = bytearray(rest)
                text = line_bytes.decode("utf-8", errors="ignore").strip()
                if not text or not text.startswith("{"):
                    continue
                try:
                    msg = json.loads(text)
                except json.JSONDecodeError:
                    continue
                self._dispatch(msg)

    def _dispatch(self, msg: Dict[str, Any]) -> None:
        mtype = msg.get("type")
        if not isinstance(mtype, str):
            return
        with self._cond:
            self._seq += 1
            if mtype == "result":
                tid = int(msg.get("id", -1))
                msg["_wall_ts"] = time.monotonic()
                self._results[tid] = msg
                # Actualizar estado de prueba de fallo guiada si coincide el ID
                if self._failure_job and self._failure_job.get("task_id") == tid:
                    self._failure_job["done"] = True
                    self._failure_job["result"] = {
                        "id": tid,
                        "status": msg.get("status"),
                        "executor": msg.get("executor"),
                        "local": msg.get("local"),
                        "latency_ms": int((time.monotonic() - self._failure_job["started_wall"]) * 1000),
                        "data_hex": msg.get("data_hex", ""),
                    }
            self._last_by_type[mtype] = (self._seq, msg)
            self._cond.notify_all()

    def _send_and_wait(
        self, command: str, expected_types: Tuple[str, ...], timeout: float = 3.0
    ) -> Dict[str, Any]:
        if not self.connected or self._ser is None:
            raise RuntimeError("ESP32 no conectado. Selecciona un puerto COM primero.")

        with self._cmd_lock:
            with self._cond:
                before_seq = self._seq
            with self._write_lock:
                self._ser.write((command.strip() + "\n").encode("ascii"))

            deadline = time.monotonic() + timeout
            with self._cond:
                while True:
                    for t in expected_types:
                        entry = self._last_by_type.get(t)
                        if entry is not None and entry[0] > before_seq:
                            return entry[1]
                    if self._stop_evt.is_set():
                        raise RuntimeError(f"Conexión perdida: {self._last_error or 'desconectado'}")
                    rem = deadline - time.monotonic()
                    if rem <= 0:
                        raise TimeoutError(f"Sin respuesta del ESP32 para '{command.split()[0]}'")
                    self._cond.wait(timeout=min(0.2, rem))

    def ping(self, timeout: float = 2.0) -> Dict[str, Any]:
        return self._send_and_wait("PING", ("pong",), timeout=timeout)

    def snapshot(self, timeout: float = 3.0) -> Dict[str, Any]:
        snap = self._send_and_wait("SNAPSHOT", ("snapshot",), timeout=timeout)
        snap["port"] = self._port_name
        snap["failure_job"] = self._failure_job
        return snap

    def set_resources(self, cpu: Optional[float] = None, mem: Optional[float] = None) -> Dict[str, Any]:
        if cpu is None or mem is None:
            return self._send_and_wait("SETRES AUTO", ("ok", "error"))
        return self._send_and_wait(f"SETRES {cpu:.3f} {mem:.3f}", ("ok", "error"))

    def set_strategy(self, strategy: int) -> Dict[str, Any]:
        return self._send_and_wait(f"STRATEGY {int(strategy)}", ("ok", "error"))

    def set_drop(self, pct: int) -> Dict[str, Any]:
        return self._send_and_wait(f"DROP {int(pct)}", ("ok", "error"))

    def reset(self) -> Dict[str, Any]:
        with self._cond:
            self._results.clear()
            self._failure_job = None
        return self._send_and_wait("RESET", ("ok", "error"))

    def submit_async(
        self,
        kind: int,
        data: bytes = b"",
        req_cpu: float = 0.0,
        req_mem: float = 0.0,
        timeout_ms: int = 0,
    ) -> Tuple[int, float]:
        if len(data) > 160:
            raise ValueError("Máximo 160 bytes por tarea")
        hex_str = data.hex().upper() if data else "-"
        cmd = f"SUBMIT {int(kind)} {hex_str}"
        if req_cpu > 0 or req_mem > 0 or timeout_ms > 0:
            cmd += f" {req_cpu:.3f} {req_mem:.3f} {int(timeout_ms)}"

        t_wall = time.monotonic()
        ack = self._send_and_wait(cmd, ("submit_ack",), timeout=3.0)
        if not ack.get("ok"):
            raise RuntimeError(f"ESP32 rechazó la tarea: {ack.get('msg')} (err {ack.get('err')})")
        return int(ack["id"]), t_wall

    def wait_result(self, task_id: int, t_wall: float, timeout: float = 35.0) -> Dict[str, Any]:
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                res = self._results.pop(task_id, None)
                if res is not None:
                    raw = bytes.fromhex(res.get("data_hex", ""))
                    latency_ms = int((res.get("_wall_ts", time.monotonic()) - t_wall) * 1000)
                    return {
                        "id": task_id,
                        "status": int(res.get("status", 1)),
                        "executor": res.get("executor", ""),
                        "local": bool(res.get("local", False)),
                        "latency_ms": max(1, latency_ms),
                        "data": raw,
                        "data_hex": res.get("data_hex", ""),
                    }
                if self._stop_evt.is_set():
                    raise RuntimeError("Conexión serie interrumpida mientras se esperaba resultado")
                rem = deadline - time.monotonic()
                if rem <= 0:
                    raise TimeoutError(f"Timeout esperando resultado de tarea {task_id}")
                self._cond.wait(timeout=min(0.25, rem))

    def run_task(
        self,
        kind: int,
        data: bytes = b"",
        req_cpu: float = 0.0,
        req_mem: float = 0.0,
        timeout_ms: int = 0,
        wait_timeout: float = 35.0,
    ) -> Dict[str, Any]:
        tid, t_wall = self.submit_async(kind, data, req_cpu, req_mem, timeout_ms)
        return self.wait_result(tid, t_wall, timeout=wait_timeout)

    def run_image_gray(self, rgb: bytes) -> Dict[str, Any]:
        if len(rgb) % 3 != 0:
            raise ValueError("Buffer RGB incompleto (debe ser múltiplo de 3)")
        out = bytearray()
        workers: List[str] = []
        latencies: List[int] = []
        t0 = time.monotonic()
        for i in range(0, len(rgb), 159):
            chunk = rgb[i : i + 159]
            res = self.run_task(3, chunk, wait_timeout=15.0)
            if res["status"] != 0:
                raise RuntimeError(f"Fragmento de imagen falló (status={res['status']})")
            out.extend(res["data"])
            workers.append(res["executor"] + (" (USB)" if res["local"] else " (ESP-NOW)"))
            latencies.append(res["latency_ms"])
        return {
            "gray": bytes(out),
            "workers": workers,
            "fragments": len(workers),
            "total_ms": int((time.monotonic() - t0) * 1000),
            "avg_fragment_ms": round(sum(latencies) / max(1, len(latencies)), 1),
        }

    def run_regression(self, points: List[List[float]]) -> Dict[str, Any]:
        if len(points) < 2:
            raise ValueError("Se necesitan al menos 2 puntos (x, y)")
        sums = [0.0] * 5
        workers: List[str] = []
        t0 = time.monotonic()
        for i in range(0, len(points), 20):
            block = points[i : i + 20]
            payload = b"".join(struct.pack("<ff", float(x), float(y)) for x, y in block)
            res = self.run_task(4, payload, wait_timeout=15.0)
            if res["status"] != 0 or len(res["data"]) != 40:
                raise RuntimeError(f"Fragmento de regresión falló (status={res['status']})")
            partial = struct.unpack("<5d", res["data"])
            sums = [a + b for a, b in zip(sums, partial)]
            workers.append(res["executor"] + (" (USB)" if res["local"] else " (ESP-NOW)"))
        n, sx, sy, sxx, sxy = sums
        den = n * sxx - sx * sx
        if not n or abs(den) < 1e-12:
            raise ValueError("Datos insuficientes o X constante")
        m = (n * sxy - sx * sy) / den
        b = (sy - m * sx) / n
        mse = sum((float(y) - m * float(x) - b) ** 2 for x, y in points) / len(points)
        return {
            "slope": m,
            "intercept": b,
            "mse": mse,
            "workers": workers,
            "fragments": len(workers),
            "total_ms": int((time.monotonic() - t0) * 1000),
        }

    def start_failure_demo(self, duration_ds: int = 100) -> Dict[str, Any]:
        """
        Prepara y lanza una tarea larga (default 10 s) forzada hacia el ESP32 #2:
        - Si hay un vecino activo, baja temporalmente C/M del nodo local (0.20, 0.20)
          para garantizar que el scheduler elija al ESP32 #2 por fitness.
        - Envía la tarea asíncronamente y devuelve de inmediato para que el usuario
          pueda desconectar físicamente el ESP32 #2 y observar la recuperación en vivo.
        """
        snap = self.snapshot()
        active_neighbors = [nb for nb in snap.get("neighbors", []) if nb.get("active")]
        if not active_neighbors:
            raise RuntimeError(
                "No se detecta el 2.º ESP32 por ESP-NOW todavía. "
                "Asegúrate de que el ESP32 #2 esté encendido con el mismo firmware y espera 3 s."
            )

        # Garantizar que el ESP32 #2 tenga mayor fitness que el local
        self.set_resources(0.20, 0.20)
        time.sleep(0.1)

        dur = max(40, min(200, int(duration_ds)))
        tid, t_wall = self.submit_async(1, bytes([dur]), timeout_ms=25000)
        self._failure_job = {
            "task_id": tid,
            "duration_s": dur / 10.0,
            "started_wall": t_wall,
            "done": False,
            "result": None,
            "instruction": (
                f"Tarea #{tid} ({dur/10.0:.0f} s) enviada al ESP32 #2. "
                "¡Desconecta ahora la alimentación del ESP32 #2! "
                "En ~6 s el ESP32 #1 detectará NEIGHBOR_DOWN y la reasignará a sí mismo."
            ),
        }
        return self._failure_job

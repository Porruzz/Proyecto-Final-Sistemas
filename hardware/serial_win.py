"""
serial_win.py - Transporte serie USB sin dependencias externas para Windows.

Usa pyserial si está instalado; de lo contrario utiliza directamente la API Win32
(kernel32.dll via ctypes) para comunicarse con puertos COM del ESP32 sin requerir
`pip install`. Desactiva DTR y RTS al abrir el puerto para no reiniciar el ESP32.
"""

from __future__ import annotations

import ctypes
import re
import sys
from typing import List, Dict

try:
    import serial as _pyserial  # type: ignore
    from serial.tools import list_ports as _pyserial_ports  # type: ignore
    HAS_PYSERIAL = True
except ImportError:
    _pyserial = None
    _pyserial_ports = None
    HAS_PYSERIAL = False


def _port_sort_key(port_name: str):
    m = re.search(r"(\d+)$", port_name.upper())
    num = int(m.group(1)) if m else 9999
    # Poner COM1 al final porque suele ser un puerto legado de la placa madre
    return (1 if port_name.upper() == "COM1" else 0, num, port_name)


def list_serial_ports() -> List[Dict[str, str]]:
    """Lista los puertos serie disponibles como [{'port': 'COM3', 'desc': '...'}]."""
    if HAS_PYSERIAL and _pyserial_ports is not None:
        items = [
            {"port": p.device, "desc": p.description or p.device}
            for p in _pyserial_ports.comports()
        ]
        items.sort(key=lambda x: _port_sort_key(x["port"]))
        return items

    if sys.platform != "win32":
        return []

    import winreg

    items: List[Dict[str, str]] = []
    try:
        key = winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM"
        )
        try:
            idx = 0
            while True:
                try:
                    dev_name, com_name, _ = winreg.EnumValue(key, idx)
                    label = dev_name.split("\\")[-1]
                    if "Silabser" in dev_name:
                        label = f"CP210x ({label})"
                    elif "CH34" in dev_name.upper() or "VCP" in dev_name.upper():
                        label = f"USB-Serial ({label})"
                    items.append({"port": str(com_name), "desc": label})
                    idx += 1
                except OSError:
                    break
        finally:
            winreg.CloseKey(key)
    except OSError:
        pass

    items.sort(key=lambda x: _port_sort_key(x["port"]))
    return items


if sys.platform == "win32":
    from ctypes import wintypes

    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    FILE_ATTRIBUTE_NORMAL = 0x80
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    PURGE_TXCLEAR = 0x0004
    PURGE_RXCLEAR = 0x0008
    CLRDTR = 6
    CLRRTS = 4

    class _DCB(ctypes.Structure):
        _fields_ = [
            ("DCBlength", wintypes.DWORD),
            ("BaudRate", wintypes.DWORD),
            ("fFlags", wintypes.DWORD),
            ("wReserved", wintypes.WORD),
            ("XonLim", wintypes.WORD),
            ("XoffLim", wintypes.WORD),
            ("ByteSize", wintypes.BYTE),
            ("Parity", wintypes.BYTE),
            ("StopBits", wintypes.BYTE),
            ("XonChar", ctypes.c_char),
            ("XoffChar", ctypes.c_char),
            ("ErrorChar", ctypes.c_char),
            ("EofChar", ctypes.c_char),
            ("EvtChar", ctypes.c_char),
            ("wReserved1", wintypes.WORD),
        ]

    class _COMMTIMEOUTS(ctypes.Structure):
        _fields_ = [
            ("ReadIntervalTimeout", wintypes.DWORD),
            ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
            ("ReadTotalTimeoutConstant", wintypes.DWORD),
            ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
            ("WriteTotalTimeoutConstant", wintypes.DWORD),
        ]

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    _k32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    _k32.CreateFileW.restype = wintypes.HANDLE

    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.CloseHandle.restype = wintypes.BOOL

    _k32.SetupComm.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD]
    _k32.SetupComm.restype = wintypes.BOOL

    _k32.GetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(_DCB)]
    _k32.GetCommState.restype = wintypes.BOOL

    _k32.SetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(_DCB)]
    _k32.SetCommState.restype = wintypes.BOOL

    _k32.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.POINTER(_COMMTIMEOUTS)]
    _k32.SetCommTimeouts.restype = wintypes.BOOL

    _k32.PurgeComm.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _k32.PurgeComm.restype = wintypes.BOOL

    _k32.EscapeCommFunction.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _k32.EscapeCommFunction.restype = wintypes.BOOL

    _k32.ReadFile.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
    ]
    _k32.ReadFile.restype = wintypes.BOOL

    _k32.WriteFile.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
    ]
    _k32.WriteFile.restype = wintypes.BOOL


class SerialPort:
    """Wrapper minimalista de puerto serie compatible con Win32 nativo y pyserial."""

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 0.1):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self._closed = False
        self._py = None
        self._handle = None

        if HAS_PYSERIAL and _pyserial is not None:
            ser = _pyserial.Serial()
            ser.port = port
            ser.baudrate = baudrate
            ser.timeout = timeout
            ser.write_timeout = 1.0
            ser.dtr = False
            ser.rts = False
            ser.open()
            ser.reset_input_buffer()
            self._py = ser
            return

        if sys.platform != "win32":
            raise RuntimeError(
                "Instala pyserial (pip install pyserial) para usar puertos serie fuera de Windows."
            )

        dev_path = f"\\\\.\\{port}" if not port.startswith("\\\\.\\") else port
        h = _k32.CreateFileW(
            dev_path,
            GENERIC_READ | GENERIC_WRITE,
            0,
            None,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            None,
        )
        if h == INVALID_HANDLE_VALUE or h is None:
            err = ctypes.get_last_error()
            raise OSError(
                f"No se pudo abrir {port} (error Win32 {err}). "
                "Verifica que el Monitor Serie de Arduino IDE esté cerrado."
            )

        try:
            _k32.SetupComm(h, 8192, 4096)

            dcb = _DCB()
            dcb.DCBlength = ctypes.sizeof(_DCB)
            if not _k32.GetCommState(h, ctypes.byref(dcb)):
                raise OSError(f"GetCommState falló en {port} (err {ctypes.get_last_error()})")

            dcb.BaudRate = baudrate
            dcb.ByteSize = 8
            dcb.Parity = 0      # NOPARITY
            dcb.StopBits = 0    # ONESTOPBIT
            dcb.fFlags = 0x0001  # fBinary=1, DTR=DISABLE, RTS=DISABLE, sin flow control
            if not _k32.SetCommState(h, ctypes.byref(dcb)):
                raise OSError(f"SetCommState falló en {port} (err {ctypes.get_last_error()})")

            _k32.EscapeCommFunction(h, CLRDTR)
            _k32.EscapeCommFunction(h, CLRRTS)

            timeouts = _COMMTIMEOUTS(
                ReadIntervalTimeout=20,
                ReadTotalTimeoutMultiplier=0,
                ReadTotalTimeoutConstant=max(10, int(timeout * 1000)),
                WriteTotalTimeoutMultiplier=0,
                WriteTotalTimeoutConstant=1500,
            )
            if not _k32.SetCommTimeouts(h, ctypes.byref(timeouts)):
                raise OSError(f"SetCommTimeouts falló en {port}")

            _k32.PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR)
        except Exception:
            _k32.CloseHandle(h)
            raise

        self._handle = h

    def read(self, size: int = 1024) -> bytes:
        if self._closed:
            return b""
        if self._py is not None:
            return self._py.read(size)
        buf = ctypes.create_string_buffer(size)
        got = wintypes.DWORD(0)
        ok = _k32.ReadFile(self._handle, buf, size, ctypes.byref(got), None)
        if not ok:
            raise OSError(f"Error leyendo {self.port} (err {ctypes.get_last_error()})")
        return buf.raw[: got.value]

    def write(self, data: bytes) -> int:
        if self._closed:
            raise OSError("Puerto serie cerrado")
        if self._py is not None:
            return int(self._py.write(data))
        written = wintypes.DWORD(0)
        ok = _k32.WriteFile(self._handle, data, len(data), ctypes.byref(written), None)
        if not ok:
            raise OSError(f"Error escribiendo en {self.port} (err {ctypes.get_last_error()})")
        return int(written.value)

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._py is not None:
            try:
                self._py.close()
            except Exception:
                pass
            self._py = None
        if self._handle is not None:
            try:
                _k32.CloseHandle(self._handle)
            except Exception:
                pass
            self._handle = None

@echo off
cd /d "%~dp0"
echo Iniciando ComplexNet Dashboard (Modo Hardware ESP-WROOM-32)...
python hardware\dashboard_esp32.py %*
pause

# cpp-psi-monitor — Pressure Stall Information monitor

Monitor de **Pressure Stall Information (PSI)** del kernel Linux en **C++23** puro (solo biblioteca estándar). Lee `/proc/pressure/{cpu,memory,io}` — la métrica del kernel (4.20+, `CONFIG_PSI`) que mide el porcentaje de tiempo que las tareas estuvieron bloqueadas esperando por CPU, memoria o I/O — y lo presenta como tabla con umbrales de color, junto con load average, uptime y número de núcleos como contexto.

## Características

- **Lectura de PSI**: `/proc/pressure/cpu`, `/proc/pressure/memory` y `/proc/pressure/io`, con métricas `some` (algunas tareas bloqueadas) y `full` (todas bloqueadas)
- **Ventanas temporales**: `avg10`, `avg60` y `avg300` (% de stall en 10s, 60s y 5min) + `total` (tiempo de stall acumulado en microsegundos, formateado como 16.3B / 876.7M / 88.2K)
- **Umbrales de color**: verde < 10%, amarillo 10–50%, rojo ≥ 50% (clasificación de presión), con aviso `⚠ HIGH PRESSURE` / `⚠ elevated pressure` / `✓ no pressure detected`
- **Parsing estricto**: valores malformados rechazan la línea completa (sin silenciar errores)
- **Contexto del sistema**: load average (`/proc/loadavg`, con procesos running/total y último PID), uptime e idle acumulado (`/proc/uptime`), núcleos de CPU (`/proc/stat`)
- **Modo watch**: `--watch N` refresca cada N segundos con limpieza de pantalla y salida limpia con Ctrl+C
- **Modo JSON**: `--json` emite la estructura completa en JSON válido para scripting
- **Self-test**: `--selftest` con 13 casos (parser de líneas PSI, umbrales, loadavg, formatos)
- **Cero dependencias**: compilación directa con `g++` sin CMake, sin librerías externas

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o psi-monitor
```

Requiere GCC 14+ (usa `std::println`, `std::from_chars`). Solo biblioteca estándar.

## Uso

```bash
# Una lectura
./psi-monitor

# Modo watch: refrescar cada 2 segundos (Ctrl+C para salir)
./psi-monitor --watch 2

# Salida JSON para scripting
./psi-monitor --json

# Sin colores (pipes, scripts, logs)
./psi-monitor --no-color

# Self-tests (13 casos)
./psi-monitor --selftest

# Ayuda
./psi-monitor --help
```

## Ejemplo

```
$ ./psi-monitor
psi-monitor — Pressure Stall Information (PSI) monitor (C++23)

  RESOURCE TYPE    AVG10   AVG60  AVG300     STALL
  cpu      some    0.00%    0.00%    0.00%     16.3B us
  cpu      full    0.00%    0.00%    0.00%         0 us
  memory   some    0.00%    0.00%    0.00%    876.7M us
  memory   full    0.00%    0.00%    0.00%    760.9M us
  io       some    0.25%    0.08%    0.01%      8.0B us
  io       full    0.25%    0.08%    0.01%      7.1B us

  load   0.90  0.84  0.70     1/713 procs    last pid 1268461
  uptime 20d 07:44:22        idle 67d 12:51:01 (4 cores)

  ✓ no pressure detected — max avg10 0.25%
```

## ¿Qué es PSI?

PSI (Pressure Stall Information) es una métrica del kernel Linux (4.20+, activable con `CONFIG_PSI`) que cuantifica la **contención de recursos** en tiempo real: el porcentaje de tiempo que las tareas se quedaron sin progresar por falta de CPU, memoria o I/O. A diferencia del load average (que cuenta procesos en cola), PSI mide el *tiempo real de stall* — el indicador que Google usa para dimensionar autoscaling. `some` = al menos una tarea bloqueada; `full` = todas las tareas de la cola bloqueadas. Valores sostenidos > 10% sugieren presión relevante; > 50% indican saturación severa.

## Exit codes

- `0` — ok (lectura correcta, selftest pasado)
- `2` — PSI no disponible (kernel < 4.20 o `CONFIG_PSI` desactivado) o argumentos inválidos

## Estructura

```
src/main.cpp          # todo el programa (parser + render + selftest)
```

## Verificación

```bash
bash /opt/data/skills/software-development/cpp-modern-dev/scripts/verify-cpp-cli.sh
# (BINARY=./psi-monitor SLUG=psi-monitor) → 9 checks, 0 fallos
```

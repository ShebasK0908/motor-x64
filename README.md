# MotorX64 (v0.1)

Un motor de CPU x86-64 que funciona dentro de un iPhone: un intérprete que
ejecuta código máquina real de PC/PS5, instrucción a instrucción, sin JIT.

La app corre 4 programas x86-64 de prueba (suma, Fibonacci recursivo, tabla
en memoria, multiplicación/división) y muestra el resultado de cada uno:
197.546 instrucciones ejecutadas en total.

## Qué es y qué no es

- **Es** la primera piedra de un emulador: el motor que entiende el idioma
  x86-64 de los juegos de PS5. Funciona a velocidad de intérprete.
- **No es** todavía un emulador de PS5: faltan la capa del sistema de la
  consola (núcleo/HLE) y la parte gráfica (GPU). Hoy no existen para iPhone
  en ningún proyecto del mundo.

## Estructura

- `core/` — el motor (`x64core.c/.h`), C puro y portable.
- `host/` — arnés de pruebas para PC.
- `tests/` — los programas x86-64 de prueba (ensamblador + binarios).
- `MotorX64/` — la app de iPhone (SwiftUI + el motor en C).
- `build.sh` — compila la `.ipa` sin firmar en un Mac / GitHub Actions.

## Instalación

Descarga `MotorX64.ipa` de la Release `motor-v1` e instálala con SideStore.

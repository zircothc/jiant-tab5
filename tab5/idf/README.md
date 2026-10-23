# JIANT en el Tab5: proyecto ESP-IDF (paso 2)

GPL-3.0-only. Necesita ESP-IDF >= 5.4 (probado con **v5.5.5**, GCC 14.2 RISC-V) y un checkout de `isod89/jiant-fm1`
que contenga este directorio `tab5/`.

    cd tab5/idf
    idf.py -DJIANT_BOARD=null set-target esp32p4 build     # sin drivers: compila, mide y arranca en un P4 cualquiera
    idf.py -DJIANT_BOARD=bsp  set-target esp32p4 build     # el Tab5 real (descarga espressif/m5stack_tab5_noglib)
    idf.py -p PUERTO flash monitor

`gen_headers.sh` (python3 + Pillow) genera al configurar las cabeceras que necesita el firmware.

## Qué hay

| Pieza | Qué hace |
|---|---|
| `components/jiant` | compila `tab5_platform.c` (el firmware completo) como componente |
| `boards/null` | placa sin drivers; el audio se marca como un sumidero real para que los tiempos sean realistas |
| `boards/bsp` | placa Tab5 real sobre el BSP (**sin verificar**, ver abajo), con `usb_midi_host.c`: MIDI USB host propio |
| `main/main.c` | tareas: audio (núcleo 1), pantalla ×2 a 30 Hz, entrada 200 Hz, persistencia a la partición `jiantnor` |
| `panel_ui.c` (en `tab5/`) | el panel completo: pantalla ×2, 27 teclas, 14 botones, 8 knobs, LEDs, multitáctil, botones enganchables |

## Verificado (compilado y enlazado para esp32p4)

- **Binario 815 KB** en una partición de app de 3 MB (74 % libre).
- **RAM interna 259 KB de 576 KB (45 %)**, PSRAM 437 KB, flash 733 KB (.text 317 KB, .rodata 415 KB).
- El `.bss` del firmware (606 KB) **no cabía en la RAM interna**: `dly_buf`, `punch_ring`, `tab5_ring`, `tab5_fb` y
  `cv_px` (todos de acceso secuencial) van a PSRAM renombrando sus secciones con `objcopy` en `components/jiant`.
- La rotación de pantalla y su inversa en el táctil (`rotation_test.c`): exactas para los dos sentidos.

## MIDI
`usb_midi_host.c` abre cualquier interfaz MIDIStreaming (clase 1, subclase 3) con endpoint IN, mantiene una transferencia
en vuelo y entrega cada paquete de 4 bytes. `main.c` los mete en una cola; la tarea de entrada (la única que toca el estado
del panel) los pasa a `panel_midi_in()`, que aplica lo asignado por MIDI learn y deja el resto para el firmware
(`tab5_midi_in()`, un anillo sin bloqueos que el propio dispositivo vacía en su hilo). El mapa de aprendizaje (200 bytes)
se guarda en NVS.

## Audio de salida
Ganancia por software (3 dB por paso, 0 a 8, guardada en NVS) con limitador suave, aplicada a lo que genera el firmware antes de
escribir al códec. Con auriculares: altavoz apagado (pin 1 del expansor 0x43, el *enable* del amplificador), −12 dB y volumen
del códec 85 en lugar de 100. La detección del jack es el pin 7 de ese mismo expansor.

## NO verificado (hay que probarlo en el Tab5)
- **El jack:** el pin 7 y su polaridad (nivel alto = enchufado) salen del BSP propio de M5Stack, y dar por hecho que la salida de
  auriculares del códec sigue activa con el amplificador del altavoz apagado sale de cómo se reparten las salidas del ES8388
  en las notas de ESPHome para el Tab5. Ninguna de las dos cosas la he podido comprobar con el aparato. Si es al revés,
  `BOARD_HP_ACTIVE_LOW`; si con el jack puesto no sale sonido por él, dímelo con el log.
- **El volumen:** el nivel por defecto (4, +12 dB) es una estimación sin haberlo oído.
- **El driver USB MIDI** compila contra las cabeceras reales de `usb_host` sin avisos, pero **nunca ha hablado con un
  dispositivo**. Tampoco se ha probado el guardado del mapa en NVS ni la alimentación del puerto USB-A.

- **`boards/bsp/board_bsp.c` no se ha compilado** (sus dependencias del registro no eran accesibles donde lo construí).
  Está escrito contra el código del BSP v1.3.2; espera arreglar algún nombre en el primer build. Dos puntos concretos:
  `esp_lcd_touch_get_coordinates` puede estar marcada obsoleta (versión 2 del componente táctil), y el códec se abre
  a 32 bit (si falla, ábrelo a 16 y convierte en `board_audio_write`).
- **Nada se ha ejecutado en el P4**: ni arranque, ni audio, ni pantalla. Todo lo anterior es de compilación y enlace.
- **Sentido de la pantalla.** El panel es 720x1280 vertical y la UI es horizontal. Si sale boca abajo, compila con
  `-DBOARD_ROTATE_B` (el táctil lo sigue).
- **Carga de CPU.** El P4 corre el firmware desde flash por caché. La tarea de audio registra cada 5 s el tiempo medio y
  el peor de cómputo frente al presupuesto por bloque (5,8 ms), y el medidor del propio firmware; mira ese log primero.
- **Escribir en flash congela las cachés**: puede cortar el audio unos ms. La tarea de persistencia espera a que pare el
  secuenciador (o hasta 5 s) para guardar. Si molesta: más tampón de I2S o guardar solo al parar.
- Hay tres revisiones de placa Tab5 (ILI9881C+GT911, ST7123, ST7121); el BSP las detecta, pero solo has de probar la tuya.

# SLOOP en el Tab5: capa de plataforma (paso 1)

**Probado con SLOOP commit `d315ad88c5fbaad232ce9426b06db66395bd87db`** (v2.6). Las cabeceras generadas de `tab5/gen/`
son de ese commit: con otra versión, borra `tab5/gen/*.h` y se regeneran (necesita Pillow). Empieza por `EMPEZAR.md`.

Licencia GPL-3.0-only, como el firmware. SLOOP es de @isod89 (basado en FELUCCA, Leo Kuroshita / Hügelton Instruments).
El diseño sigue el build web de FELUCCA (`web/emu/felucca_web.c`, a su vez según charlesvestal/fm1-x0x).

**No se ha modificado nada de `firmware/src` ni de `firmware/hal`.** Todo vive en `tab5/`.

## Qué es

`tab5_platform.c` compila el firmware como una sola unidad (igual que `src/felucca.c`), pero sustituye cada cabecera
de registros por dobles pequeños. Un solo hilo, con el reloj del dispositivo marcado por el audio que se pide:

| Hardware del FM-1 | En la plataforma |
|---|---|
| TIMER4 / reloj | `tab5_step_ms()` avanza 1 ms |
| ISR de audio ALNK0 | la ISR real (`fm1_alnk0_irq`) se llama por cada mitad I2S pendiente |
| matriz de teclas, 7 encoders, ADC de MASTER | máscaras de bits, contadores de pasos y un valor 0..1023 |
| LEDs | `fm1_led`, `fm1_led_dim`, `fm1_led_bg`, consultables por control |
| LCD ST7789 240x240 | `lcd_fill`/`lcd_blit` a un framebuffer RGB565 |
| SPI NOR | imagen de 1 MiB en RAM tras los hooks de `storage.c` |
| USB, MIDI TRS, editor, OTA, consola | no incluidos (no se arrancan) |

## Compilar y probar en el PC

Copia `tab5/` a la raíz de un checkout de `isod89/sloop-fm1` y, desde la raíz:

    tab5/build_host.sh                          # genera las cabeceras y compila en build/tab5/
    mkdir -p /tmp/t5 && build/tab5/tab5_host /tmp/t5          # prueba completa; escribe PPM, WAV y nor.bin
    build/tab5/tab5_host /tmp/t5 --resume                     # arranca desde la flash guardada

Necesita gcc, python3 y Pillow (para las cabeceras generadas). Se compila con `-O2` (con `-O0` las funciones
`always_inline` de los HAL emiten ensamblador del JieLi).

## La interfaz (`tab5_platform.h`)

    tab5_power_on();                      // arranque
    tab5_audio_read(dst, n);              // n frames estéreo, int32 de 24 bit justificados a la izquierda (formato del códec)
    tab5_buttons(mask); tab5_keys(mask);  // controles mantenidos: botón por etiqueta, tecla k = F3 + k semitonos
    tab5_enc(role, steps); tab5_master(v);
    tab5_lit_*() / tab5_glow_*() / tab5_back_*()   // LEDs: encendido, glow, retroiluminación
    tab5_screen(); tab5_screen_draws();   // 240x240 RGB565 (byte alto primero) y contador de repintado

## Qué comprueba el arnés (todo en verde)

arranque con splash, silencio sin teclas, sonido al pulsar y silencio al soltar, el LED de la tecla y del botón
mantenidos, intervalo de octava exacto (ratio 2,006) y orden de notas, la pantalla reacciona a SEQ y al KNOB 1,
programar 3 pasos desde el panel, autosave a flash en reposo (17 escrituras), PLAY/STOP, MASTER a 0 y a máximo,
y **tras reiniciar desde la imagen de flash los 3 pasos siguen ahí**.

## Límites conocidos

- Solo probado en x86-64 con gcc 13. **No se ha ejecutado todavía en el ESP32-P4.**
- `fm1_ticks()` es el reloj determinista, no uno real: el medidor de CPU (`cpu_q8`) marca 0 y el apagado de voces por
  sobrecarga no actúa. En el Tab5 asigna `tab5_ticks_hook` a un temporizador real.
- La prueba de afinación solo verifica octava y orden: el parche por defecto está desafinado/con coro y una
  estimación de una sola nota fluctúa unas décimas de semitono.
- UBSan señala desplazamientos a la izquierda de valores negativos en el firmware original (comportamiento definido en gcc).

## Pruebas en el PC (`build_host.sh` construye las tres)
- `tab5_host`: el firmware tal cual (audio, pantalla, LEDs, flash, reinicio).
- `panel_test`: el panel táctil con dedos simulados (multitáctil, enganche, knobs, MASTER).
- `panel_midi_test`: paquetes USB-MIDI y MIDI learn (asignar, toggle, relativo/absoluto, persistencia, borrar).

## Paso 2 (hecho): ESP-IDF

Ver `idf/README.md`: el firmware compila y enlaza para ESP32-P4 (815 KB, 45 % de la RAM interna), con tareas de
audio, pantalla ×2, entrada y persistencia. La placa real (`boards/bsp`) está escrita pero sin compilar.

## Siguiente (paso 3)

El panel táctil completo (14 botones, 27 teclas, 8 knobs, LEDs) con el layout propuesto, multitáctil y botones
enganchables con pulsación larga.

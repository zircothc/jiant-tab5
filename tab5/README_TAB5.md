# JIANT FM en el M5Stack Tab5

Port de [JIANT FM](https://github.com/juanjiant-bit/JIANT) (firmware del sintetizador M-Vave FM-1, fork de Felucca) al **M5Stack Tab5** (ESP32-P4). Toda la interfaz va en la pantalla táctil: 14 botones, 7 encoders, 27 teclas, multitoque, MIDI USB host, audio por el códec ES8388 (altavoz o jack).

## Cómo compilar
1. Clona JIANT y copia la carpeta `tab5` dentro (o clona este repositorio).
2. ESP-IDF v6.1, VS Code con la extensión ESP-IDF. Abre `tab5/idf`.
3. Set Device Target → `esp32p4`, y Build / Flash / Monitor.

## Notas
- La capa de plataforma (`tab5_platform.c`, `panel_ui.c`) sustituye los registros del JieLi del FM-1 por código para el Tab5. El firmware de JIANT no se modifica.
- Pantalla y táctil girados 180° (`BOARD_ROTATE_B`).
- El monitor serie imprime una línea DIAG cada 5 s y el uso de CPU del audio.

## Licencia
GPL-3.0-only, igual que JIANT y Felucca. Créditos: JIANT FM © Juan Ignacio Antonucci, basado en FELUCCA © Leo Kuroshita / Hügelton.

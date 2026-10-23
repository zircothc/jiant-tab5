# JIANT FM en el Tab5 — pasos

1. Descarga/clona JIANT: `git clone https://github.com/juanjiant-bit/JIANT.git`
2. Descomprime este paquete **encima** de la carpeta JIANT (así se crea/actualiza `JIANT/tab5`). No borres nada.
3. Abre `JIANT/tab5/idf` en VS Code (Trust Folder).
4. Set Device Target → `esp32p4`. Build, Flash, Monitor (la primera compilación tarda; no la interrumpas).

Pantalla y táctil ya vienen girados 180°. Cada 5 s el monitor serie imprime una línea DIAG.
No probado aún en el Tab5 real: compilación ESP-IDF y hardware.

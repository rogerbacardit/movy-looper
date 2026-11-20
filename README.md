# Looper 606

Looper de 6 pistas que se carga **dentro de una pista de Movy** como instrumento (Schwung `sound_generator`, entrada de audio activada). Los pads controlan grabar / reproducir / sobregrabar y los knobs son el mezclador.

El motor de audio es **Mark** de Tim Cox (MIT, https://github.com/timncox/schwung-mark), incluido como fork propio en `src/mark_core.c` (MIT, (c) Tim Cox; modificado para Looper 606, ver `LICENSE-mark`). Las ideas de uso vienen de la Boss RC-505 y de Loopex (solo ideas, ningun codigo). Detalles del diseno en `docs/BRIEF.md`.

## Mapa de pads (4 filas x 8 columnas)

Pad 1 = abajo a la izquierda. Fila 1 = la de abajo.

```
        MASTER   T1     T2     T3     T4     T5     T6     AJUSTES REC
fila 4  UNDO/    FX     FX     FX     FX     FX     FX     Count-in
        REDO     on/off on/off on/off on/off on/off on/off on/off
fila 3  ARM      MUTE   MUTE   MUTE   MUTE   MUTE   MUTE   Rec Action
        (rev.)                                              REC>PLAY/DUB
fila 2  STOP     PLAY/  PLAY/  PLAY/  PLAY/  PLAY/  PLAY/  Largo fijo
        TODO     STOP   STOP   STOP   STOP   STOP   STOP   libre/1/2/4/8
fila 1  TODO     REC/   REC/   REC/   REC/   REC/   REC/   Quantize
        START/   DUB    DUB    DUB    DUB    DUB    DUB    on/off
        STOP
```

Colores: verde = reproduciendo, rojo = grabando, violeta = overdub, naranja parpadeante = ARM/armada. Las filas MUTE/FX (y STOP, parada) usan el color de cada pista (azul, cian, amarillo, magenta, blanco, naranja); mute = gris. La columna 1 y la 8 no usan ni verde ni rojo.

- **REC/DUB** (fila 1): pista vacia -> graba; segundo toque -> cierra la grabacion (PLAY, o DUB si Rec Action = REC>DUB); PLAY -> DUB; DUB -> PLAY; pista parada -> empieza a sonar directamente en DUB. Se ve en rojo (rec) / violeta (dub).
- **PLAY/STOP** (fila 2): pista parada -> PLAY; sonando/DUB -> STOP; grabando -> cierra la grabacion y para. **Borrar pista (CLEAR): mantener Delete (X) del Move y tocar cualquier pad de la columna de esa pista** (Movy lo reenvia al modulo con `delete_pad_param`). UNDO lo deshace; UNDO otra vez = rehacer el borrado; UNDO es de un solo nivel. Verde = suena, color de pista = parada con clip, apagado = vacia.
- **MUTE**: silencia la pista pero sigue girando, y al quitar el mute vuelve en su sitio.
- **ARM** (columna 1, fila 3): un toque activa/desactiva el modo "grabar al detectar sonido": al pulsar REC en una pista vacia queda *armada* y empieza a grabar sola cuando la entrada supera el umbral. Tocar REC otra vez cancela. ARMADA parpadea en naranja.
- **REVERSE**: toca ARM y, en menos de 1 s, el pad de la **fila 2** de una pista: esa pista cambia de sentido y ARM vuelve a como estaba. Un pad de la fila 1 en esa ventana se ignora (nunca graba ni hace dub).

Todos los gestos actuan al note-on.
- **Count-in**: al grabar en una pista vacia espera 4 tiempos antes de empezar.
- **Largo fijo**: cada toque cicla libre -> 1 -> 2 -> 4 -> 8 compases; la grabacion se cierra sola.

## Knobs (paginas)

- **Mezclador (pagina inicial):** K1-K6 volumen de las pistas 1-6, K7 volumen de entrada, K8 master.
- **Track** (pista = el ultimo pad tocado): tipo de FX, cantidad de FX, pan, reverse, one-shot, velocidad (1/2x, 1x, 2x, estilo cinta: cambia el tono; funciona con reverse; no se sobregraba con velocidad distinta de 1x).
- **Setup:** umbral de ARM, quantize, largo fijo, Rec Action, count-in, BPM manual (0 = automatico), monitor, arm.
- **Status:** estado de cada pista (solo lectura): Empty / Armed / Rec / Play / Dub / Stop / Mute. Los pads no tienen luces, la pantalla es el unico aviso.

## Compilar y probar

```
make test-synth     # compila para el Mac y ejecuta los tests (132 comprobaciones)
make arm-synth      # compila para la Move -> build/looper-606-module.tar.gz
```

Para `arm-synth` hace falta `aarch64-linux-gnu-gcc` (toolchain `aarch64-linux-gnu`, ya instalado con brew en este Mac) o Docker en marcha. Segundos maximos por pista: `make arm-synth MARK_SECONDS=60` (por defecto 120; 6 pistas x 120 s + undo ~ 148 MB; si calloc falla el motor baja solo a 90/60/45/30...).

Nota tecnica: el motor soporta `MARK_TRACKS=6` de forma nativa (ya no hay parche con sed). Los cambios respecto a Mark estan marcados `L606` en `src/mark_core.c`.

## Instalacion en la Move

1. `tar -xzf build/looper-606-module.tar.gz` (crea la carpeta `looper-606/` con `dsp.so` y `module.json`).
2. Copiala a la Move: `scp -r looper-606 ableton@move.local:/data/UserData/schwung/modules/sound_generators/`
3. Ajusta el propietario: `ableton:users` (`chown -R ableton:users` en esa carpeta).
4. Reinicia Schwung / vuelve a escanear modulos. En Movy, carga **Looper 606** como sonido de una pista.

## Puntos por verificar en el dispositivo

- Tempo: el log dice una sola vez `bpm source = ...` (override / MIDI clock / host get_bpm() / default 120). Sin reloj MIDI se usa `get_bpm()`; quantize nunca bloquea una grabacion (a los 1,5 compases + 0,5 s arranca sola).
- Entrada de audio: el log dice `input source = ...` al crear e `input peak after 1 s = N`. Si pone SILENT, el host no entrega audio en el buzon: el modulo no puede arreglarlo (ver docs/BRIEF.md, "Entrada de audio en Movy standalone").
- Keep-alive: Movy duerme una pista cuya salida lleva ~1 s en silencio; mientras haya pistas en uso el modulo suma un dither de +-6 LSB (-74 dBFS) para que el looper siga corriendo.
- Que el orden de notas de los pads coincida (el log guarda las primeras 8 notas pulsadas; pad 1 = nota 36).

## Pad colours and status text in Movy (needs the `module-leds` Movy build)

The shim publishes two read-only params and names them in the `ui_hierarchy`
root (`pad_leds_param`, `status_text_param`, `display_param`, `delete_pad_param`):

- `pad_leds`: 64 hex digits, one byte per rack pad (pad 1 first). Bits 0-6 are a
  Move LED palette index, bit 7 = blink. Track colours: green, blue, yellow,
  pink, cyan, orange (dim = empty, bright = playing); red = rec, violet = dub,
  dim white = stopped, grey = muted, red blink = armed.
- `status_text`: `"<seq>:<text>"` (`MUTE 2`, `REC 1`, `QUANT ON`, ...). Movy shows a
  toast when `seq` changes.

A Movy without the feature ignores both params; nothing else changes.

- `display`: `"<seq>|<pos 0-127>|<128 peaks as 256 hex digits, or empty>|<header>"`, the selected track's waveform overview (header e.g. `PLAY 1 - 4 BARS`). Peaks are cached in the module and `seq` bumps only when they change.
- `delete_pad` (write): Movy sends the rack pad index 0-31 when Delete + pad is pressed; the module CLEARs that pad's track.

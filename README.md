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
- **ARM** (columna 1, fila 3): un toque (pulsar y soltar) activa/desactiva el modo "grabar al detectar sonido": al pulsar REC en una pista vacia queda *armada* y empieza a grabar sola cuando la entrada supera el umbral. Tocar REC otra vez cancela. ARMADA parpadea en naranja.
- **REVERSE**: manten pulsado ARM y toca cualquier pad de una pista (fila 1 o fila 2): esa pista cambia de sentido (nunca graba, reproduce ni para). Al soltar ARM tras usarlo asi, ARM no cambia. Mientras ARM esta pulsado, los pads de fila 2 se iluminan en blanco (invertida) o color de pista (normal).

Todos los gestos actuan al note-on.
- **Count-in**: al grabar en una pista vacia espera 4 tiempos antes de empezar.
- **Largo fijo**: cada toque cicla libre -> 1 -> 2 -> 4 -> 8 compases (desde cualquier valor pasa al siguiente de esa lista); la grabacion se cierra sola. Los 16 botones de paso eligen cualquier largo de 1 a 16 (ver abajo).

## Botones de paso = largo de grabacion (build `module-leds` v3 de Movy)

Los 16 botones bajo los pads: el paso N fija un largo de N compases para las proximas grabaciones; volver a pulsar el paso elegido = largo libre. Los LEDs muestran el largo (paso N blanco, 1..N-1 tenues, resto apagado; libre = todo apagado) y el aviso es `LEN 4 BARS` / `LEN FREE`. Es el mismo ajuste que el pad de la columna 8 y que `Fixed Length` de Setup (que ahora llega a 16 compases). En una pista con este modulo los pasos NO son del secuenciador de Movy (Shift+paso, Mute+paso y la vista Session siguen igual).

## Loop + knob = inicio del clip

Mantener el boton **Loop** del Move y girar el knob N (1-6) mueve el punto de inicio de la pista N, sin destruir el audio: el motor reproduce desde `(pos + inicio) mod largo` y el overdub escribe en ese mismo punto, asi que las capas siguen alineadas (vale con reverse y velocidad). Cada paso del knob = 1/16 de nota al tempo actual; con **Shift** = 1/64 (fino, fuera de la rejilla; sin Shift vuelve a la rejilla). Se envuelve en ambos sentidos y hay un iman en el inicio del loop. La pantalla muestra la onda con una marca de inicio y el titular `START 1 +3/16` (o `+N MS` en fino) unos segundos; la marca queda visible mientras el inicio no sea 0. No se puede mover mientras la pista graba o hace overdub (`START 1 LOCKED`) ni si esta vacia. CLEAR + UNDO devuelve tambien el inicio. Con Loop pulsado Movy no entra en modo Loop en esta pista (es solo un modificador).

## Cursor de la onda

El cursor de la pantalla sigue la posicion real: con la pista en reverse va de derecha a izquierda, con velocidad 2x/0.5x va al doble/mitad y con inicio movido sigue el audio que suena.

## Persistencia de los loops

Los loops sobreviven a cerrar/recargar Movy y a apagar la Move.

- Movy guarda por pista y por Set lo que el modulo publica en `get_param("state")` (en `chains.json` del Set) y lo devuelve con `set_param("state")` al cargar el Set, pero no dice al modulo ni el Set ni la pista. Por eso el `state` lleva un **ID de loops** aleatorio (`l6id`, 12 hex) creado con cada instancia, y los loops viven en `/data/UserData/schwung/looper-loops/<id>/` (`t1.wav`..`t6.wav` + `loops.json` con largos y puntos de inicio). Es la particion de datos de usuario: no se borra al actualizar Movy ni Schwung.
- **Nunca en el hilo de audio**: un hilo propio mira cada 100 ms una firma barata de cada pista y guarda las que cambiaron cuando llevan 1,5 s estables (no se escribe durante REC/DUB). CLEAR borra el `.wav` (UNDO lo vuelve a escribir). Se escribe a `.tmp` y se renombra, `loops.json` el ultimo. Al cerrar la instancia (`destroy`) se vuelca lo pendiente.
- **Al cargar** un Set cuyo `state` trae un ID, y solo si la instancia esta vacia, el mismo hilo lee los WAV y deja las pistas en STOP (nunca pisa loops vivos). Ademas del audio vuelven largo fijo, velocidad, mute, count-in, umbral y volumen de entrada.
- Limitaciones: un Set duplicado por Movy comparte ID con el original hasta que uno grabe algo nuevo; quitar el modulo de una pista deja su carpeta huerfana (se puede borrar a mano); una carpeta ilegible no se sobrescribe. Variables de entorno solo para tests: `LOOPER606_PERSIST_DIR`, `LOOPER606_PERSIST_DELAY_MS`.

## Knobs (paginas)

- **Mezclador (pagina inicial):** K1-K6 volumen de las pistas 1-6, K7 volumen de entrada, K8 master.
- **Track** (pista = el ultimo pad tocado): tipo de FX, cantidad de FX, pan, reverse, one-shot, velocidad (1/2x, 1x, 2x, estilo cinta: cambia el tono; funciona con reverse; no se sobregraba con velocidad distinta de 1x).
- **Setup:** umbral de ARM, quantize, largo fijo, Rec Action, count-in, BPM manual (0 = automatico), monitor, arm.
- **Status:** estado de cada pista (solo lectura): Empty / Armed / Rec / Play / Dub / Stop / Mute. Los pads no tienen luces, la pantalla es el unico aviso.

## Compilar y probar

```
make test-synth     # compila para el Mac y ejecuta los tests (346 comprobaciones)
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

The shim publishes read-only/write-only params and names them in the `ui_hierarchy`
root (`pad_leds_param`, `status_text_param`, `display_param`, `delete_pad_param`,
and since v0.4.0 `step_buttons_param`, `step_leds_param`, `loop_knob_param`):

- `pad_leds`: 64 hex digits, one byte per rack pad (pad 1 first). Bits 0-6 are a
  Move LED palette index, bit 7 = blink. Track colours: green, blue, yellow,
  pink, cyan, orange (dim = empty, bright = playing); red = rec, violet = dub,
  dim white = stopped, grey = muted, red blink = armed.
- `status_text`: `"<seq>:<text>"` (`MUTE 2`, `REC 1`, `QUANT ON`, ...). Movy shows a
  toast when `seq` changes.

A Movy without the feature ignores both params; nothing else changes.

- `display`: `"<seq>|<pos 0-127>|<128 peaks as 256 hex digits, or empty>|<header>"`, the selected track's waveform overview (header e.g. `PLAY 1 - 4 BARS`). Peaks are cached in the module and `seq` bumps only when they change.
- `delete_pad` (write): Movy sends the rack pad index 0-31 when Delete + pad is pressed; the module CLEARs that pad's track.
- `step`: write-only, step button index 0..15 (Movy sends it instead of a sequencer step).
- `step_leds`: 32 hex digits, one palette byte per step button.
- `loop_knob`: write-only `"N:delta[:1]"` while Loop is held (N = knob 1..8, `:1` = Shift/fine).
- `display`: the pos field is `"<pos>"` or `"<pos>,<start marker column>"`.

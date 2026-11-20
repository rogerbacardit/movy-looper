# Looper 606

Looper de 6 pistas que se carga **dentro de una pista de Movy** como instrumento (Schwung `sound_generator`, entrada de audio activada). Los pads controlan grabar / reproducir / sobregrabar y los knobs son el mezclador.

El motor de audio es **Mark** de Tim Cox (MIT, https://github.com/timncox/schwung-mark), incluido como fork propio en `src/mark_core.c` (MIT, (c) Tim Cox; modificado para Looper 606, ver `LICENSE-mark`). Las ideas de uso vienen de la Boss RC-505 y de Loopex (solo ideas, ningun codigo). Detalles del diseno en `docs/BRIEF.md`.

## Mapa de pads (4 filas x 8 columnas)

Pad 1 = abajo a la izquierda. Fila 1 = la de abajo.

```
        MASTER   T1     T2     T3     T4     T5     T6     AJUSTES REC
fila 4  SHIFT    SELEC  SELEC  SELEC  SELEC  SELEC  SELEC  Count-in
        (mant.)  pista  pista  pista  pista  pista  pista  on/off
fila 3  ARM      MUTE   MUTE   MUTE   MUTE   MUTE   MUTE   Rec Action
        (rev.)                                              REC>PLAY/DUB
fila 2  STOP     PLAY/  PLAY/  PLAY/  PLAY/  PLAY/  PLAY/  Largo fijo
        TODO     STOP   STOP   STOP   STOP   STOP   STOP   libre/1/2/4/8
fila 1  TODO     REC/   REC/   REC/   REC/   REC/   REC/   Quantize
        START/   DUB    DUB    DUB    DUB    DUB    DUB    on/off
        STOP
```

**UNDO / REDO** ya no es un pad: es el boton ↩ Undo del Move (y Shift+Undo = rehacer) cuando la pista activa es el Looper (Movy lo reenvia con `undo_param`); en las demas pistas Undo sigue siendo el de Movy. Deshace el ultimo overdub, grabacion, CLEAR o cambio de largo (un nivel).

**Selector de pista (fila 4, columnas 2-7) y vista PISTA.** Tocar el pad de una pista la selecciona: los 8 knobs pasan a ser de esa pista (Movy los captura con `knob_capture_param`) y la onda la sigue. Tocar la pista ya seleccionada vuelve a MIX (knobs = volumenes, como siempre). Cada pad usa el color de la pista, tenue; la seleccionada, brillante (nunca verde ni rojo). La pantalla muestra `PISTA 3` y el ultimo valor tocado, y cada giro lanza un aviso tipo `T3 IN +8/16`. Con la vista PISTA activa, los pedales 3/4 mueven tambien la seleccion, y los pads de otras pistas no cambian la pista de la onda.

| Knob | Vista PISTA (pista seleccionada) |
|---|---|
| K1 | IN: inicio del clip (rejilla 1/16; SHIFT = fino 1/64). Mientras lo giras (y 2,5 s despues) la onda hace ZOOM: ventana de 2 compases centrada en IN (1/2 compas con SHIFT), cabecera `PISTA 3 IN +8/16 ZOOM` |
| K2 | OUT: fin/largo del loop (rejilla 1/16; SHIFT = fino 1/64; nunca menos de 1/16; acortar no destruye; una racha de giros = un paso de Undo; la onda muestra todo lo grabado con marcas de inicio y fin; mientras lo giras hace ZOOM de 2 compases centrado en OUT, igual que K1) |
| K3 | Volumen de la pista |
| K4 | Pan |
| K5 | Tipo de FX: OFF / LPF / HPF / CRUSH / DELAY / PHASER / RING (3 clics por paso) |
| K6 | Cantidad de FX |
| K7 | Velocidad x1/4, x1/2, x3/4, x1, x1.5, x2, x4 (3 clics por paso; por defecto x1; sin overdub si no es x1) |
| K8 | Reverse off / on (3 clics) |

**Onda:** en vista PISTA (sin tocar IN/OUT) se ve todo el buffer con las marcas IN/OUT; en MIX solo la region que suena (IN..OUT) a todo el ancho, con los compases contados desde IN, asi que el clip cambia de tamano al recortarlo. Al mover IN/OUT (K1/K2 o Loop + knob) la vista hace zoom sobre ese punto (2 compases, 1/2 con fino; el buffer entero si el clip es mas corto) y vuelve sola 2,5 s despues de la ultima vuelta. Los estados guardados antiguos (velocidad 1/2x, 1x, 2x) se cargan como x1/2, x1, x2.

En MIX (nada seleccionado): K1-K6 volumenes de las pistas, K7 entrada, K8 master. Ya no existe el pad MODO (START/END/PAN/FX global desaparecen: sus funciones estan en los knobs de la vista PISTA).

**SHIFT** (pad de arriba de la columna 1, mantener):

| SHIFT + | Efecto |
|---|---|
| K1 / K2 (en vista PISTA) | IN / OUT fino (1/64) |
| pad fila 4 de una pista | FX on/off de esa pista (mantiene tipo y cantidad; mientras SHIFT esta pulsado el pad se ve blanco si tiene FX) |
| pad fila 1 de una pista | Reverse de esa pista (nunca graba) |
| pad fila 3 de una pista | SOLO: silencia las demas; otra vez = quita el solo y restaura los mutes anteriores (el pad de la pista en solo parpadea) |
| columna 8 fila 4 | Count-in: cicla 1 / 2 compases |
| columna 8 fila 3 | Umbral de ARM: -54 / -48 / -42 / -36 / -30 dB (aviso `THR -36 DB`) |
| columna 8 filas 2 y 1 | sin funcion por ahora |

El pad SHIFT es gris tenue y blanco mientras se mantiene.

Colores: verde = reproduciendo, rojo = grabando, violeta = overdub, naranja parpadeante = ARM/armada. Las filas MUTE/selector (y STOP, parada) usan el color de cada pista (azul, cian, amarillo, magenta, blanco, naranja); mute = gris. La columna 1 y la 8 no usan ni verde ni rojo.

- **REC/DUB** (fila 1): pista vacia -> graba; segundo toque -> cierra la grabacion (PLAY, o DUB si Rec Action = REC>DUB); PLAY -> DUB; DUB -> PLAY; pista parada -> empieza a sonar directamente en DUB. Se ve en rojo (rec) / violeta (dub).
- **PLAY/STOP** (fila 2): pista parada -> PLAY; sonando/DUB -> STOP; grabando -> cierra la grabacion y para. **Borrar pista (CLEAR): mantener Delete (X) del Move y tocar cualquier pad de la columna de esa pista** (Movy lo reenvia al modulo con `delete_pad_param`). El boton **Undo** del Move lo deshace; Shift+Undo = rehacer el borrado; UNDO es de un solo nivel. Verde = suena, color de pista = parada con clip, apagado = vacia.
- **MUTE**: silencia la pista pero sigue girando, y al quitar el mute vuelve en su sitio.
- **ARM** (columna 1, fila 3): un toque (pulsar y soltar) activa/desactiva el modo "grabar al detectar sonido": al pulsar REC en una pista vacia queda *armada* y empieza a grabar sola cuando la entrada supera el umbral. Tocar REC otra vez cancela. ARMADA parpadea en naranja.
- **REVERSE**: SHIFT + pad de fila 1 (o K8 en la vista PISTA). Alias antiguo: manten pulsado ARM y toca un pad de pista (fila 1 o 2); al soltar ARM tras usarlo asi, ARM no cambia, y mientras esta pulsado los pads de fila 2 se iluminan en blanco (invertida) o color de pista (normal).

Todos los gestos actuan al note-on.
- **Count-in**: al grabar en una pista vacia espera 1 compas (4 tiempos) antes de empezar; SHIFT + el pad lo cambia a 2 compases (no se guarda).
- **Largo fijo**: cada toque cicla libre -> 1 -> 2 -> 4 -> 8 compases (desde cualquier valor pasa al siguiente de esa lista); la grabacion se cierra sola. Los 16 botones de paso eligen cualquier largo de 1 a 16 (ver abajo).

## Botones de paso = largo (build `module-leds` v4 de Movy)

Los 16 botones bajo los pads actuan sobre la **pista seleccionada** (el ultimo pad de pista tocado):
- **Con clip**: el paso N pone el loop a N compases (al tempo actual). Mas corto = recorte no destructivo (el audio se conserva; alargar de nuevo lo recupera); mas largo que lo grabado = el contenido se repite hasta llenarlo (estilo RC-505, hasta lo que cabe en la pista: si no, `LEN 1: TOO LONG`). Overdub, reverse, velocidad e inicio siguen alineados (el inicio se reajusta al nuevo largo) y el nuevo largo se guarda con los loops. Aviso `LEN 1: 4 BARS`. Undo (boton del Move) devuelve el largo anterior.
- **Pista vacia**: el paso N fija un largo de N compases para las proximas grabaciones; volver a pulsar el paso elegido = largo libre. Aviso `LEN 4 BARS` / `LEN FREE`.

Los LEDs muestran el largo del clip seleccionado, o el de las proximas grabaciones si esta vacia (paso N blanco, 1..N-1 tenues, resto apagado; libre = todo apagado). Es el mismo ajuste que el pad de la columna 8 y que `Fixed Length` de Setup (que ahora llega a 16 compases). En una pista con este modulo los pasos NO son del secuenciador de Movy (Shift+paso, Mute+paso y la vista Session siguen igual).

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
- **Track** (pista = el ultimo pad tocado): tipo de FX, cantidad de FX, pan, reverse, one-shot, velocidad (x1/4, x1/2, x3/4, x1, x1.5, x2, x4, estilo cinta: cambia el tono; funciona con reverse; no se sobregraba con velocidad distinta de x1).
- **Setup:** umbral de ARM, quantize, largo fijo, Rec Action, count-in, BPM manual (0 = automatico), monitor, arm.
- **Status:** estado de cada pista (solo lectura): Empty / Armed / Rec / Play / Dub / Stop / Mute. Los pads no tienen luces, la pantalla es el unico aviso.

## Compilar y probar

```
make test-synth     # compila para el Mac y ejecuta los tests (585 comprobaciones)
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

## Pedalera Blackstar Live Logic

Pedalera en el hub USB-A del Move, en modo **CUSTOM** (no PATCH). Necesita el build `module-leds` de Movy, que reenvia los 6 interruptores al looper con `pedal_param` (`pedal` = `"<n>:d"` / `"<n>:u"`). Siempre actua sobre la pista seleccionada del looper (`cur_track`, la que siguen la pantalla y la onda); el aviso es `TRACK n`.

| Interruptor | Toque | Mantener 1 s |
|---|---|---|
| 1 | REC/DUB/PLAY de la pista seleccionada (como el pad de la fila 1) | - |
| 2 | PLAY/STOP de la pista seleccionada (como el pad de la fila 2) | CLEAR de la pista seleccionada (se puede deshacer) |
| 3 | pista anterior (1..6, da la vuelta) | ARM auto-rec on/off (a 1 s, sin soltar) |
| 4 | pista siguiente (da la vuelta) | - |
| 5 | UNDO (como el boton Undo del Move) | - |
| 6 | STOP ALL si algo suena, si no ALL START | - |

Los interruptores 1, 4, 5 y 6 actuan al pisar. El 2 y el 3 tienen gesto de mantener, asi que su toque actua al soltar (si se solto antes de 1 s); un mantenido nunca dispara ademas el toque.

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
- `knob_capture` (read, 0/1) and `knob_delta` (write `"N:delta[:1]"`): while `knob_capture` is 1 for the active track, Movy sends every knob turn (Move and Twister) to `knob_delta` instead of the hierarchy.
- `undo_cmd` (write `"undo"` / `"redo"`): Move's Undo / Shift+Undo on this track.
- `display`: the pos field may be `"<pos>,<start>,<end>"` (end marker; -1 = none).
- `display`: the pos field is `"<pos>"` or `"<pos>,<start marker column>"`.

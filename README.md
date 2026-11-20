# Movy Looper

Looper de 6 pistas que se carga **dentro de una pista de Movy** como instrumento (Schwung `sound_generator`, entrada de audio activada). Los pads controlan grabar / reproducir / sobregrabar y los knobs son el mezclador.

El motor de audio es **Mark** de Tim Cox (MIT, https://github.com/timncox/schwung-mark), incluido sin modificar en `src/mark_core.c` (ver `LICENSE-mark`). Las ideas de uso vienen de la Boss RC-505 y de Loopex (solo ideas, ningun codigo). Detalles del diseno en `docs/BRIEF.md`.

## Mapa de pads (4 filas x 8 columnas)

Pad 1 = abajo a la izquierda. Fila 1 = la de abajo.

```
        MASTER   T1     T2     T3     T4     T5     T6     AJUSTES REC
fila 4  UNDO/    FX     FX     FX     FX     FX     FX     Count-in
        REDO     on/off on/off on/off on/off on/off on/off on/off
fila 3  ARM      MUTE   MUTE   MUTE   MUTE   MUTE   MUTE   Rec Action
        (rev.)                                              REC>PLAY/DUB
fila 2  STOP     STOP   STOP   STOP   STOP   STOP   STOP   Largo fijo
        TODO     (*)    (*)    (*)    (*)    (*)    (*)    libre/1/2/4/8
fila 1  TODO     REC    REC    REC    REC    REC    REC    Quantize
        START/   PLAY   PLAY   PLAY   PLAY   PLAY   PLAY   on/off
        STOP     DUB    DUB    DUB    DUB    DUB    DUB
```

- **REC** (fila 1): pista vacia -> graba; segundo toque -> reproduce (o sobregraba si Rec Action = REC>DUB); luego alterna PLAY/DUB.
- **STOP** (*): toque corto = parar. **Mantener 1 segundo = borrar la pista** (CLEAR). Ojo: el borrado no se puede deshacer (el motor Mark no lo permite).
- **MUTE**: silencia la pista pero sigue girando, y al quitar el mute vuelve en su sitio.
- **ARM** (columna 1, fila 3): un toque activa/desactiva el modo "grabar al detectar sonido": al pulsar REC en una pista vacia queda *armada* y empieza a grabar sola cuando la entrada supera el umbral. Tocar REC otra vez cancela.
- **REVERSE**: mantener ARM pulsado y tocar el REC de una pista (ARM no cambia).
- **Count-in**: al grabar en una pista vacia espera 4 tiempos antes de empezar.
- **Largo fijo**: cada toque cicla libre -> 1 -> 2 -> 4 -> 8 compases; la grabacion se cierra sola.

## Knobs (paginas)

- **Mezclador (pagina inicial):** K1-K6 volumen de las pistas 1-6, K7 volumen de entrada, K8 master.
- **Track** (pista = el ultimo pad tocado): tipo de FX, cantidad de FX, pan, reverse, one-shot. (Velocidad 1/2x-2x no existe en el motor y se ha omitido.)
- **Setup:** umbral de ARM, quantize, largo fijo, Rec Action, count-in, BPM manual (0 = automatico), monitor, arm.
- **Status:** estado de cada pista (solo lectura): Empty / Armed / Rec / Play / Dub / Stop / Mute. Los pads no tienen luces, la pantalla es el unico aviso.

## Compilar y probar

```
make test-synth     # compila para el Mac y ejecuta los tests (67 comprobaciones)
make arm-synth      # compila para la Move -> build/movy-looper-module.tar.gz
```

Para `arm-synth` hace falta `aarch64-linux-gnu-gcc` (toolchain `aarch64-linux-gnu`, ya instalado con brew en este Mac) o Docker en marcha. Segundos maximos por pista: `make arm-synth MARK_SECONDS=60` (por defecto 30; 6 pistas x 60 s ~ 74 MB).

Nota tecnica: el motor solo enruta las pistas 1-5; `scripts/patch-core.sh` genera `build/mark_core6.c` (copia con 2 lineas ampliadas a 6 pistas). `src/mark_core.c` queda intacto.

## Instalacion en la Move

1. `tar -xzf build/movy-looper-module.tar.gz` (crea la carpeta `movy-looper/` con `dsp.so` y `module.json`).
2. Copiala a la Move: `scp -r movy-looper ableton@move.local:/data/UserData/schwung/modules/sound_generators/`
3. Ajusta el propietario: `ableton:users` (`chown -R ableton:users` en esa carpeta).
4. Reinicia Schwung / vuelve a escanear modulos. En Movy, carga **Looper** como sonido de una pista.

## Puntos por verificar en el dispositivo

- Que llegue reloj MIDI / BPM al modulo (en el log aparece "first MIDI clock received"; si no, usa BPM manual).
- Que llegue audio de entrada (log "audio_in is live").
- Que el orden de notas de los pads coincida (el log guarda las primeras 8 notas pulsadas; pad 1 = nota 36).

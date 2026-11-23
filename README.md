# Looper 606

**📖 Manual interactivo (8 idiomas): https://rogerbacardit.github.io/movy-looper/**

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

**Pagina de la vista PISTA (v0.7.0).** Con una pista seleccionada, la pantalla del Move muestra los 8 knobs como una pagina de parametros estilo Schwung (2 filas x 4: dial redondo para valores continuos, caja con texto para opciones, etiqueta corta debajo; titulo `PISTA 3` a la izquierda y el estado, p. ej. `PLAY`, a la derecha; el ultimo knob girado muestra su valor 1,5 s en lugar de la etiqueta). Mientras giras IN/OUT (y 2,5 s despues) se ve la onda con zoom; luego vuelve la pagina. En MIX sigue la onda. Requiere Movy `module-leds` v8 (el modulo lo publica con `knob_page_param`/`knob_page`).

| Knob | Vista PISTA (pista seleccionada) |
|---|---|
| K1 | IN: inicio del clip (rejilla 1/16; sin modo fino: SHIFT no cambia el paso). Mientras lo giras (y 2,5 s despues) la onda hace ZOOM: ventana de 2 compases centrada en IN (1/2 compas con Loop + knob fino), cabecera `PISTA 3 IN +8/16 ZOOM` |
| K2 | OUT: fin/largo del loop (rejilla 1/16, sin fino; nunca menos de 1/16; acortar no destruye; una racha de giros = un paso de Undo; la onda muestra todo lo grabado con marcas de inicio y fin; mientras lo giras hace ZOOM de 2 compases centrado en OUT, igual que K1) |
| K3 | Volumen de la pista |
| K4 | Pan |
| K5 | Tipo de FX: OFF / LPF / HPF / CRUSH / DELAY / PHASER / RING (3 clics por paso) |
| K6 | Cantidad de FX |
| K7 | Velocidad x1/4, x1/2, x3/4, x1, x1.5, x2, x4 (3 clics por paso; por defecto x1; sin overdub si no es x1) |
| K8 | Reverse off / on (3 clics) |

**Onda:** en vista PISTA (sin tocar IN/OUT) se ve todo el buffer con las marcas IN/OUT; en MIX solo la region que suena (IN..OUT) a todo el ancho, con los compases contados desde IN, asi que el clip cambia de tamano al recortarlo. Al mover IN/OUT (K1/K2 o Loop + knob) la vista hace zoom sobre ese punto (2 compases, 1/2 con fino; el buffer entero si el clip es mas corto) y vuelve sola 2,5 s despues de la ultima vuelta. Los estados guardados antiguos (velocidad 1/2x, 1x, 2x) se cargan como x1/2, x1, x2.

En MIX (nada seleccionado): K1-K6 volumenes de las pistas, K7 entrada, K8 master. Ya no existe el pad MODO (START/END/PAN/FX global desaparecen: sus funciones estan en los knobs de la vista PISTA).

**SHIFT** (pad de arriba de la columna 1). Toque corto (< 300 ms, sin usar ningun combo) = activa/desactiva el **modo PERFORM**; mantenido = modificador momentaneo:

| SHIFT + | Efecto |
|---|---|
| K1 / K2 (en vista PISTA) | sin efecto especial (pasos normales; SHIFT tendra una segunda pagina de knobs) |
| pad fila 4 de una pista | FX CLR de esa pista: apaga sus efectos PERFORM, FX de pista OFF, reverse off, velocidad 1x y borra su automatizacion (audio, volumen, pan y mute intactos). Es el gesto fiable, vive solo en el modulo |
| pad fila 1 de una pista | Reverse de esa pista (nunca graba) |
| pad fila 3 de una pista | SOLO: silencia las demas; otra vez = quita el solo y restaura los mutes anteriores (el pad de la pista en solo parpadea) |
| columna 8 fila 4 | Count-in: cicla 1 / 2 compases |
| columna 8 fila 3 | Umbral de ARM: -54 / -48 / -42 / -36 / -30 dB (aviso `THR -36 DB`) |
| columna 8 filas 2 y 1 | sin funcion por ahora |

| pad fila 1 de la columna 8 | MONITOR on/off (aviso `MONITOR ON/OFF`) |

El pad SHIFT es gris tenue, blanco mientras se mantiene y blanco parpadeante con el modo PERFORM activo. Mantener SHIFT o usar cualquier combo (incl. girar un knob) NO cambia el modo PERFORM al soltar.

Colores: verde = reproduciendo, rojo = grabando, violeta = overdub, naranja parpadeante = ARM/armada. Las filas MUTE/selector (y STOP, parada) usan el color de cada pista (azul, cian, amarillo, magenta, blanco, naranja); mute = gris. La columna 1 y la 8 no usan ni verde ni rojo.

- **REC/DUB** (fila 1): pista vacia -> graba; segundo toque -> cierra la grabacion (PLAY, o DUB si Rec Action = REC>DUB); PLAY -> DUB; DUB -> PLAY; pista parada -> empieza a sonar directamente en DUB. Se ve en rojo (rec) / violeta (dub).
- **PLAY/STOP** (fila 2): pista parada -> PLAY; sonando/DUB -> STOP; grabando -> cierra la grabacion y para. **Borrar: mantener Delete (X) del Move y tocar un pad de la columna de la pista** (Movy lo reenvia al modulo con `delete_pad_param`). **X + pad de las filas 1-3 = borra el audio** (CLEAR; la pista conserva sus ajustes). **X + el selector PISTA (fila superior) = pista desde cero** (RESET: borra el audio y devuelve FX, volumen 100, pan centro, velocidad 1x, reverse, one-shot y mute a los valores de fabrica, y la saca del solo). El boton **Undo** del Move lo deshace (en RESET devuelve tambien los ajustes; solo si la pista tenia audio); Shift+Undo = rehacer; UNDO es de un solo nivel. El pedal (mantener 2) solo borra el audio. Verde = suena, color de pista = parada con clip, apagado = vacia.
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

## PERFORM: efectos en los botones de paso (v0.11.0)

Con **SHIFT** (pad de arriba de la columna 1) mantenido, la pantalla muestra la pagina **PERFORM**: una rejilla de 4 x 4 casillas (`NN XXX`: numero y nombre de 3 letras) con los 16 efectos en el orden de los botones de paso (pasos 1-4 arriba, 13-16 abajo). El titulo lleva el objetivo: `PISTA 3` o `MIX`. Las casillas de efectos activos llevan marca: un cuadradito si estan en la pila y una banderita con visto si estan **encendidos** con un toque. Los LEDs de paso se iluminan por familia: azul = repeticion, naranja = cinta/tiempo, violeta = tono y filtros, cian = ritmo, verde azulado = espacio (delay/freeze), amarillo = KILL. Sin SHIFT ni modo PERFORM los pasos siguen fijando el largo del loop igual que siempre.

**Modo PERFORM, un boton a la vez (v0.11.0):** un toque corto de SHIFT lo activa (aviso `PERFORM ON`, SHIFT parpadea en blanco) y otro lo desactiva. Flujo: toca SHIFT, toca el selector de pista de la fila superior (PISTA; otra vez = MIX), y toca un paso:

- **Un toque (< 300 ms)** enciende el efecto y se queda encendido sin mantener; la pantalla muestra la escena 3D de ese efecto con sus casillas K1-K3 y los knobs lo controlan (tambien en MIX: `knob_capture` = 1 mientras el modo este activo y haya algun efecto encendido). **Otro toque en el mismo paso lo apaga** y vuelve la pagina rejilla, o la escena del efecto encendido anterior.
- **Mantener (> 300 ms)** = momentaneo: se apaga al soltar.
- Otro paso se **apila** (maximo 6, "PERFORM: MAX 6") y pasa a ser el efecto **actual** (pantalla y knobs). Ya no existe el doble toque de latch: el toque simple es el enclavamiento.
- SHIFT mantenido + paso con el modo apagado sigue siendo momentaneo (el efecto sigue si sueltas SHIFT antes que el paso).
- Al salir del modo (toque de SHIFT) los efectos encendidos siguen sonando y parpadean en los LEDs de paso; al volver a entrar se ve su escena. Cada efecto se queda en la pista (o MIX) donde se encendio. Sin modo, los pasos vuelven a fijar el largo del loop. Entrar en PISTA o MIX no sale del modo. No se guarda.
- Con el modo apagado, SHIFT mantenido deja los knobs sobre el efecto actual si queda alguno encendido.

**Borrar (Movy Delete + pad):** **X + selector de pista (fila superior) = CLEAR solo del audio** de esa pista (deshacer lo devuelve; ajustes y efectos no se tocan). **RESET completo** (audio, ajustes de fabrica y efectos PERFORM de esa pista): **pad SHIFT del looper (columna 1, fila 4) + X + selector**. **SHIFT nativo del Move + selector de pista = FX CLR**: apaga los efectos PERFORM de esa pista, FX de pista en OFF, reverse off, velocidad 1x y borra su automatizacion; el audio, el volumen, el pan y el mute no se tocan (aviso `FX CLR 3`; necesita la UI `ui-module-leds-v12.js`). **X + pad REC (fila 1) = borrar la ultima grabacion**: si la ultima accion de esa pista fue una pasada de overdub y el nucleo aun guarda su captura de deshacer (un nivel), solo se quita esa ultima capa (como un Undo de ese dub, aviso `UNDO DUB 3`); si no, se borra el audio del clip (`CLEAR 3`). **X + filas 2-3** siguen siendo CLEAR del audio.

**SHIFT + mantener el paso N** = suena el efecto N mientras se mantiene; al soltar vuelve todo exactamente como estaba y en tiempo. Objetivo: la pista seleccionada en vista PISTA, o toda la salida del looper en MIX (el objetivo se fija al pulsar).

**Combinar:** puedes tener hasta **6 efectos a la vez** (mantenidos o encendidos; el septimo se rechaza con "PERFORM: MAX 6"). Se apilan siempre en un orden de cadena fijo, sin importar el orden en que los pulses: tiempo/repeticion (BEAT REPEAT, STUTTER, RETRIGGER, REVERSE) → tono/cinta (PITCH, HALF SPEED, TAPE, LFO, VINYL) → filtros (LP, HP) → BITCRUSH → GATE → KILL → DELAY / FREEZE. Si dos efectos del mismo grupo de tiempo o de tono van juntos, manda el ultimo de la cadena (los dos leen la misma historia). Los knobs controlan el efecto **actual** (el encendido o pulsado mas recientemente). La escena muestra ese efecto y, a la izquierda, etiquetas pequenas con los demas.

Los efectos se aplican **solo al lado de lectura/salida** (un anillo de ~6 s con el audio de cada pista y otro de la mezcla; cada objetivo tiene su propio motor): nunca se escribe en los buffers del loop ni se mueve un playhead, asi que al soltar la mezcla es identica bit a bit a la que habria sin efecto (los tests lo comprueban). Unica excepcion: la cola de DELAY THROW, que sigue sonando tras soltar y se apaga sola; el seco vuelve exacto en cuanto termina. Entrada y salida con fundido de 4 / 12 ms (KILL 3 ms), sin clicks. El monitor de entrada en directo no pasa por los efectos.

Mientras un efecto es el actual, los 8 knobs (knob capture) son sus parametros; cada efecto recuerda sus ajustes (no se guardan al apagar). Los parametros de opciones (tasa, tiempo, tamano...) saltan una opcion por click. La pantalla muestra una escena animada en linea fina estilo HUD/radar que reacciona a los parametros y al nivel de audio, con casillas de lectura a la derecha.

| Paso | Efecto | Knobs | Escena |
|---|---|---|---|
| 1 | BEAT REPEAT: repite la rodaja justo anterior, alineada a la rejilla del loop | K1 tasa (1/4 1/8 1/16 1/32 1/8T 1/16T) · K2 decay por repeticion · K3 bajada de tono por repeticion (0..12 st) | circulos concentricos que laten en cada repeticion (n = tasa) |
| 2 | STUTTER: repeticiones que se aceleran de 1/4 a 1/32 | K1 tiempo 1..4 pulsos · K2 tasa final (1/16, 1/32, 1/64) · K3 curva de aceleracion (lineal → exponencial) | barras que convergen cada vez mas rapido |
| 3 | PITCH: desplaza el tono sin cambiar el tiempo (granular, 40 ms) | K1 -12..+12 semitonos · K2 afinacion fina ±50 cents · K3 mezcla seco/mojado | dial de radar semicircular con aguja |
| 4 | BITCRUSH | K1 bits 16..2 · K2 submuestreo 1..32 · K3 mezcla seco/mojado | anillo que se pixela + senoide cuantizada |
| 5 | TAPE (stop + start): al encenderlo la cinta frena (velocidad y tono bajan hasta 0, silencio); al apagarlo vuelve a subir de 0 a la velocidad normal en el mismo tiempo y el efecto termina. En momentaneo: pulsar = stop, soltar = start | K1 tiempo 1/4..2 pulsos · K2 curva (lineal → pronunciada) · K3 wow (fluctuacion de velocidad) | dos bobinas que frenan / aceleran |
| 6 | LFO: modulacion senoidal ciclica sincronizada a la rejilla del loop (barrido repetido, "vuelo a la luna"); entra con 30 ms de rampa y al apagar vuelve exacto | K1 tasa (1/16 1/8T 1/8 1/8. 1/4 1/4. 1/2 1 2 4 compases) · K2 destino (FILT = barrido de paso bajo, VOL, PAN, PTCH = ±7 st) · K3 profundidad | cinta senoidal 3D con un anillo que sube y baja con la fase |
| 7 | REVERSE (`BCK`): lee hacia atras desde el punto de pulsacion (hasta ~2.9 s y vuelve a empezar) | K1 ventana (1/4, 1/2, 1 compas, libre desde la pulsacion) · K2 mezcla seco/mojado (solo 2 knobs) | espirales con flechas girando al reves |
| 8 | VINYL: tono cayendo despacio como un disco que frena (no llega a parar) + ruido | K1 freno · K2 crujido · K3 wow (oscilacion lenta del tono) | disco con surcos y brazo |
| 9 | FILTER LP: paso bajo resonante (SVF de 2 polos), el cutoff sigue al knob con suavizado | K1 cutoff 30 Hz..18 kHz (exponencial) · K2 resonancia · K3 pendiente 12 / 24 dB | mira/diana con linea de corte horizontal que sube y baja, zona cortada rayada, pico de resonancia y arco de frecuencia |
| 10 | FILTER HP: igual, paso alto | K1 cutoff · K2 resonancia · K3 pendiente 12 / 24 dB | igual, rayado por debajo |
| 11 | GATE: puerta ritmica sincronizada a la rejilla del loop (abierta la primera mitad de cada celda) | K1 tasa (1/4 1/8 1/16 1/32 1/8T 1/16T) · K2 profundidad · K3 swing (retrasa las celdas impares) | rejilla de celdas que parpadean con el patron y cursor de fase |
| 12 | DELAY THROW: la entrada llega al delay solo mientras se mantiene; los ecos siguen sonando al soltar (cola) | K1 tiempo (1/8 1/8. 1/4 1/4. 1/2) · K2 feedback (0..88 %, repeticiones cada vez mas oscuras) · K3 nivel del eco (0..100 %) | anillos concentricos que se expanden, mas tenues con cada eco |
| 13 | ECHO FREEZE: congela una rodaja en un bucle infinito con crossfade (sigue sonando sin entrada) | K1 tamano (1/16 1/8 1/4 1/2 1 pulso) · K2 mezcla con el seco · K3 decay del bucle congelado (0 % = infinito) | cristal/copo de nieve que crece, mas ramas con tamano mayor |
| 14 | RETRIGGER: cada unidad de rejilla se repite su primer cuarto saltando atras al inicio de la unidad (retrigger de DJ, en tiempo) | K1 cada 1/4, 1/2, 1 compas · K2 bajada de volumen por retrigger · K3 bajada de tono por retrigger (0..4 st) | flechas de rebobinado en un anillo y cabezas de lectura/directo |
| 15 | HALF SPEED: lectura a ½ o ¼ de velocidad (cae el tono) que vuelve a tiempo cada ciclo (2 o 4 pulsos) | K1 ½ / ¼ · K2 mezcla seco/mojado (solo 2 knobs) | orbita lenta con estela y orbita fantasma a velocidad normal |
| 16 | KILL: silencio inmediato (fundido de 3 ms) mientras se mantiene | K1 parte: ALL / LOW / HIGH (24 dB/oct) · K2 fundido 2 ms..500 ms · K3 frecuencia de corte de LOW/HIGH 60 Hz..12 kHz (en ALL no hace nada; muestra -) | X grande con rayas de aviso; en LOW/HIGH, espectro con la zona muerta tachada |

**v0.10.2 (knobs):** cada efecto tiene K1..K3 reales (REVERSE y HALF SPEED solo K1-K2) y cada uno se recuerda por efecto. En pantalla solo llevan casilla los knobs que existen, con su numero en una pestana a la izquierda (`1`, `2`, `3`); los valores meramente informativos (direccion, ratio) van como texto suelto sin casilla. El indicador LATCH pasa al titulo. Los knobs llegan al efecto con `knob_delta` ("N:delta") tambien nada mas pulsar el paso y, con SHIFT mantenido, sobre efectos solo enclavados. Limite conocido (lado Movy): `knob_capture` se lee por sondeo (~40 ms); en MIX, un giro que llegue antes de que Movy vea `knob_capture=1` va a la pagina normal de knobs (mezclador) y el modulo no puede interceptarlo; en vista PISTA la captura ya esta activa y no hay retraso.

Escenas 3D: desde la v0.10.1 cada efecto tiene una escena de alambre 3D (motor minimo `src/wire3d.c`: rotaciones, proyeccion en perspectiva, recorte al visor y aristas lejanas punteadas como pista de profundidad; solo lineas de 1 px). Beat repeat = cubos anidados, stutter = losas apiladas, pitch = cubo estirado, bitcrush = esfera de anillos, tape = dos bobinas, reverse = tunel, vinyl = disco inclinado con brazo, filtros = rejilla synthwave (horizonte = cutoff, onda = reso), gate = 4x2 cubos, delay = marcos que se alejan, freeze = octaedro, retrigger = cubo que salta, half speed = esfera lenta, kill = cubo que implosiona. Titulo, lecturas a la derecha, etiquetas de efectos combinados e indicadores de latch no cambian.

Pantalla propia: el modulo declara `screen_param` (framebuffer 128x64 de 1 bit, `"<seq>|<2048 hex>"`) y Movy lo dibuja en lugar de la onda o la pagina de knobs mientras no sea `""`. Se genera en `get_param` (nunca en `process()`), a 25 fps como maximo y solo si cambia (`src/gfx1.c`, `src/perform_ui.c`). Tambien usa `step_up_param` para saber cuando se suelta un paso. Hace falta la build `module-leds` v10 de Movy o posterior (la v0.10.0 no pide cambios en Movy).

Volcar fotogramas para verlos: `make perform-frames` (PBM + PNG ampliado en `build/perform-frames/`, o `FRAMES_DIR=...`).

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
| 3 | pista anterior (1..6, da la vuelta) | ARM on/off (a 1 s, sin soltar) |
| 4 | pista siguiente (da la vuelta) | - |
| 5 | UNDO (como el boton Undo del Move) | - |
| 6 | STOP ALL si algo suena, si no ALL START | - |

Los interruptores 1, 4, 5 y 6 actuan al pisar. El 2 y el 3 tienen gesto de mantener, asi que su toque actua al soltar (si se solto antes de 1 s); un mantenido nunca dispara ademas el toque.

## Mute y transporte de Movy (v0.8.0, build `module-leds` de Movy)

El mute de pista de Movy (M + boton de pista, y el silencio que pone un solo) solo corta las notas secuenciadas; no toca el audio de un modulo. Con `track_mute_param` (`track_mute`, `"1"`/`"0"`) Movy avisa al looper: silencia toda su salida con un fundido de ~10 ms (las 6 pistas siguen corriendo en tiempo, como su MUTE) y la cabecera de la pantalla muestra `MUTED`. Movy lo escribe al cambiar y una vez al cargar el modulo.

Con `transport_param` (`transport`, `"play"`/`"stop"`) el Play/Stop de la Move controla el looper: **Stop** = STOP ALL recordando que pistas sonaban (un segundo Stop conserva la memoria); **Play** = arranca exactamente esas pistas desde el inicio del loop, a compas si quantize esta activo; sin memoria, arranca todas las pistas con audio (ALL START). Una grabacion en curso no se toca.

## Pad colours and status text in Movy (needs the `module-leds` Movy build)

The shim publishes read-only/write-only params and names them in the `ui_hierarchy`
root (`pad_leds_param`, `status_text_param`, `display_param`, `delete_pad_param`,
and since v0.4.0 `step_buttons_param`, `step_leds_param`, `loop_knob_param`):

- `pad_leds`: 64 hex digits, one byte per rack pad (pad 1 first). Bits 0-6 are a
  Move LED palette index, bit 7 = blink. Track colours (track 5 = light magenta): green, blue, yellow,
  pink, cyan, orange (dim = empty, bright = playing); red = rec, violet = dub,
  dim white = stopped, grey = muted, red blink = armed.
- `status_text`: `"<seq>:<text>"` (`MUTE 2`, `REC 1`, `QUANT ON`, ...). Movy shows a
  toast when `seq` changes.

A Movy without the feature ignores both params; nothing else changes.

- `display`: `"<seq>|<pos 0-127>|<128 peaks as 256 hex digits, or empty>|<header>"`, the selected track's waveform overview (header e.g. `PLAY 1 - 4 BARS`). Peaks are cached in the module and `seq` bumps only when they change.
- `delete_pad` (write): Movy sends the rack pad index 0-31 when Delete + pad is pressed; the module CLEARs that pad's track (audio only); with the looper SHIFT pad held on a PISTA selector it RESETs the track (audio + all settings, undoable).
- `shift_pad` (write): Movy sends the rack pad index when Move's own Shift + pad is pressed (the press is not forwarded as a note); on a PISTA selector it clears that track's effects (`FX CLR`).
- `step`: write-only, step button index 0..15 (Movy sends it instead of a sequencer step).
- `step_leds`: 32 hex digits, one palette byte per step button.
- `loop_knob`: write-only `"N:delta[:1]"` while Loop is held (N = knob 1..8, `:1` = Shift/fine).
- `knob_capture` (read, 0/1) and `knob_delta` (write `"N:delta[:1]"`): while `knob_capture` is 1 for the active track, Movy sends every knob turn (Move and Twister) to `knob_delta` instead of the hierarchy.
- `undo_cmd` (write `"undo"` / `"redo"`): Move's Undo / Shift+Undo on this track.
- `display`: the pos field may be `"<pos>,<start>,<end>"` (end marker; -1 = none).
- `display`: the pos field is `"<pos>"` or `"<pos>,<start marker column>"`.

**v0.11.0:** TAPE STOP y TAPE START se unen en el paso 5 (TAPE); el paso 6 es el nuevo LFO. Cambia el modelo de uso del modo PERFORM (toque = encender/apagar, mantener = momentaneo) y desaparece el doble toque de latch. La escena de un efecto encendido se ve en modo PERFORM aunque no haya ningun paso pulsado. Captura: `docs/screens/perform-3d-v0.11.0.png`.

**v0.12.0:** DUB se ve rojo (como REC). Delete + selector de pista solo borra el audio de esa pista; el RESET completo pasa a SHIFT + Delete + selector. En PERFORM, `<` y `>` cambian de pagina de efectos (pagina 2 = SIDECHAIN, paso 1; requiere la UI `ui-module-leds-v11.js`). Los knobs K1-K3 de un efecto PERFORM se graban como automatizacion (64 pasos sobre el loop) mientras una pista graba/hace overdub o con AUTO-REC (SHIFT + ARM) y se reproducen en bucle; Delete + knob (o ARM + knob) la borra. Columna 8 en blanco (brillante = activo / distinto del valor por defecto). Detalle en docs/BRIEF.md.

**v0.13.0:** SHIFT nativo del Move + selector de pista = FX CLR (ver Borrar). **Automatizacion de knobs, nuevo gesto:** en modo PERFORM con un efecto encendido, MANTEN el pad REC (fila 1) de una pista que suena y gira K1-K3: lo que gires se graba a lo largo de todo el loop de esa pista (`AUTO-REC 3`; durante la pulsacion REC no graba ni hace overdub). Al soltar, la automatizacion suena en bucle. Un toque corto de REC sin girar ningun knob sigue siendo el REC normal. X + knob (o ARM + knob) borra la curva de ese knob. La automatizacion se guarda con el set. Desaparece SHIFT + ARM = AUTO-REC.

**v0.14.0:**
- **X + selector de pista = borrar la AUTOMATIZACION de esa pista** (todos los FX y knobs; aviso `FX+AUTO CLR 3` (tambien apaga los efectos PERFORM de esa pista)), el audio se queda. X + filas 2-3 siguen borrando audio; X + REC = deshacer dub / borrar. RESET completo: pad SHIFT del looper + X + selector.
- **FX CLR:** pad SHIFT del looper (columna 1, fila 4) mantenido + selector de pista (fila 4) = FX CLR de esa pista (solo en el modulo; ya no hay on/off de FX en ese gesto, K5 sigue fijando el tipo y OFF). El SHIFT nativo del Move + selector hace lo mismo si Movy lo entrega (UI v13).
- **Colores:** la pista 4 pasa a verde lima (paleta 9) para no confundirse con el magenta de la pista 5.
- **Automatizacion con mas resolucion:** 256 pasos por loop, interpolacion lineal en la reproduccion, rampa lineal entre posiciones al grabar y knobs en pasos de 0.5 % mientras REC esta mantenido. El estado guardado usa base64 (342 caracteres por curva).
- **Rejilla con efectos encendidos:** `<` o `>` muestran la rejilla de la pagina (un cambio de pagina siempre la muestra; la flecha sin pagina en ese sentido la alterna). Mantener el pad SHIFT del looper (sin pulsar un paso) la muestra mientras dura. Girar un knob vuelve a la escena del efecto. Se quitan las marcas "+" de las esquinas de las escenas; en la rejilla, efecto encendido = subrayado macizo + cuadrado en la esquina (relleno = enclavado, hueco = momentaneo).
- **LFO:** K1 es un solo knob por pasos: 14 divisiones sincronizadas (1/32, 1/16T, 1/16, 1/8T, 1/8, 1/8., 1/4T, 1/4, 1/4., 1/2, 1 compas, 2, 4, 8 compases) y despues 11 tasas libres en Hz (0.05, 0.1, 0.2, 0.5, 1, 2, 4, 8, 12, 20, 32 Hz).
- **Hasta 6 efectos a la vez** (PERFORM: MAX 6). CPU medida en el test de host (-O2, un carril): sin efectos ~2 us por bloque de 128 frames; con 6 pesados (PITCH, DELAY, FLANGER, CHORUS, PHASER, COMB) ~23 us (presupuesto 2900 us).
- **Pagina 2 de efectos** (`>` en PERFORM): paso 1 SIDECHAIN, y nuevos pasos 2-9: 2 DRIVE (K1 ganancia, K2 tono, K3 mezcla), 3 RING MOD (K1 frecuencia 20 Hz..4 kHz, K2 modo FREE / NOTE: portadora libre o clavada al semitono, K3 mezcla), 4 FLANGER (K1 tasa, K2 profundidad, K3 feedback), 5 PHASER (4 etapas; K1 tasa, K2 profundidad, K3 feedback), 6 CHORUS (K1 tasa, K2 profundidad, K3 mezcla), 7 AUTOPAN (K1 tasa sincronizada 1/16..4 compases, K2 profundidad, K3 forma SIN/TRI/SQR), 8 TREMOLO (igual), 9 COMB (K1 tono 55 Hz..1.7 kHz, K2 feedback, K3 mezcla). Orden de cadena: RING MOD tras el pitch/tape; COMB, CHORUS, FLANGER, PHASER tras los filtros; DRIVE antes de BITCRUSH; AUTOPAN y TREMOLO tras SIDECHAIN. Color de los LEDs: DRIVE, RING MOD y COMB violeta (tono), FLANGER, PHASER y CHORUS verde lima (modulacion), AUTOPAN y TREMOLO cian (ritmo).

**v0.14.2:** X + selector de pista = `RESET FX n`: FX OFF y cantidad por defecto, pan centro, volumen 100, velocidad 1x, reverse off, mute off, one-shot off, efectos PERFORM de esa pista apagados y su automatizacion borrada. El audio y IN / OUT no se tocan. El RESET completo (con audio) sigue en pad SHIFT del looper + X + selector. Automatizacion: un ciclo = el clip de IN a OUT a cualquier velocidad; con reverse sigue al audio; al empezar a girar un knob con REC mantenido los pasos vacios se rellenan con el valor actual, asi la curva abarca todo el loop.

**v0.14.3:** SHIFT nativo del Move + selector de pista (fila 4) = FX CLR, y SHIFT nativo + pad MUTE de una pista (fila 3) = SOLO de esa pista (otra vez = vuelven los mutes anteriores). Es el mismo valor (indice de pad 0..31) que usa X + pad; la nota 88 / rack 25 del log es el pad MUTE de la pista 4.

**v0.14.4:** en modo PERFORM (o con SHIFT mantenido, que muestra la pagina PERFORM) los knobs nunca tocan IN / OUT ni la velocidad: con un efecto encendido van al efecto; sin efecto no hacen nada al clip. Tambien Loop + knob (`loop_knob`) se ignora.

**v0.14.5: tacto de los knobs de efectos.** Cada K1-K3 es continuo o por pasos. Un knob por pasos acumula clicks y mueve una posicion cada 10 clicks si tiene N <= 4 posiciones, cada 6 si N <= 8 y cada 3 en las listas largas (se reinicia al tocar otro knob u otro efecto o al invertir el giro) y el aviso muestra la opcion (`RATE 32HZ`). Los continuos mantienen su respuesta (0.5 % por click con REC mantenido) con una aceleracion suave solo en giros muy rapidos (delta >= 6). La automatizacion de un knob por pasos graba y reproduce posiciones exactas, sin interpolar.
Knobs por pasos (N): BEAT REPEAT K1 (6); STUTTER K2 (3); PITCH K1 (25 semitonos); BITCRUSH K1 (15 bits) y K2 (32 divisores); LFO K1 (25 tasas) y K2 (4 destinos); REVERSE K1 (4); FILTER LP / HP K3 (2); GATE K1 (6); DELAY K1 (5); FREEZE K1 (5); RETRIGGER K1 (3); HALF SPEED K1 (2); KILL K1 (3); SIDECHAIN K1 (6); RING MOD K2 (2); AUTOPAN y TREMOLO K1 (8) y K3 (3). El resto de knobs son continuos.

**v0.14.6:** en la columna 1 se intercambian STOP ALL (ahora abajo del todo, fila 1) y ALL START/STOP (fila 2). STOP ALL se enciende en amarillo cuando algo suena y queda apagado en reposo.

**v0.14.6:** el pad LEN (columna 8, fila 2) cicla Libre / 1 / 2 / 4 / 8 / 16 / 32 compases. Si el largo no cabe en el buffer de pista (120 s, depende del tempo) se recorta al mayor que cabe y avisa `LEN 32 > MAX 16`; si ya estas en ese maximo, la siguiente pulsacion vuelve a Libre. El enum de Setup llega a 32 compases y lo guardado antiguo (<= 16) sigue valiendo. Los botones de paso siguen fijando 1..16 compases; con un clip de mas de 16 compases se ven todos en gris tenue.

**v0.15.0: efecto TEMPO** (pagina 2, paso 10; color naranja, familia cinta / tiempo). Cambia el tempo de ESE clip sin cambiar el tono (time-stretch): dos granos Hann leen el buffer del loop desde una cabeza de analisis independiente que avanza a (ratio x velocidad) mientras el playhead del loop sigue a su tempo. K1 = BPM objetivo del clip, continuo, 1 BPM por click (0.1 con REC mantenido), del 50 % al 200 % del tempo del proyecto; por defecto el BPM del proyecto (ratio 1). Aviso `TEMPO 96 BPM (80%)`; la pantalla muestra el BPM en la casilla y el % pequeno bajo el metronomo 3D. K2 = glide hacia el objetivo: JUMP, 1/2 pulso, 1, 2 pulsos, 1 compas, 2 compases (sincronizado). K3 = tamano de grano 15..90 ms (calidad / transitorios). Al apagarlo vuelve a 1x con el mismo glide y se resincroniza con el playhead del loop en el siguiente pulso. Solo funciona en pistas: con MIX seleccionado avisa `TEMPO: PICK A TRACK` (estirar la mezcla exigiria una cabeza de lectura por pista). La automatizacion de K1 es continua e interpolada. Va el primero de la cadena (lee el clip, no el flujo). CPU medida en el test de host (-O2, un carril de pista): 6 efectos (PITCH, DELAY, FLANGER, CHORUS, TEMPO, COMB) unos 29 us por bloque de 128 frames (presupuesto 2900 us).

**v0.15.1 (arranque de pistas):** con quantize ON, PLAY en una pista parada espera al siguiente COMPAS de la rejilla (a 120 BPM como mucho 2 s; a 60 BPM, 4 s; a 40 BPM, 6 s; no depende del largo del loop base, medido con bases de 1, 4 y 16 compases); con quantize OFF arranca al instante. La pista que se lanza empieza en el punto del clip que la mantiene en fase (tiempo desde el inicio del loop base, a su velocidad, modulo su largo), asi un clip corto bajo un loop largo sigue sincronizado; la primera pista, un one-shot o el transporte parado empiezan desde el principio. Mientras espera el compas, el pad PLAY/STOP (fila 2) y el pad REC (fila 1) parpadean en verde (grabacion pendiente: rojo). Con el transporte en pausa el lanzamiento es inmediato (antes quedaba pendiente hasta reanudar).

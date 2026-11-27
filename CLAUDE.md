# FPVDesktopWiFiReceiver: cómo se trabaja en este repo

Receptor FPV analógico de 5,8 GHz y analizador de espectro (5,8 y 2,4 GHz) con un adaptador WiFi RTL8812BU.
C++20, Qt 6. `src/radio` y `src/kp_ext` derivan de kepyur (GPL-2.0, comentarios en inglés); el resto es de esta app.

Las prácticas siguen [`ub-mode`](https://github.com/unclebob/ub-mode) de Uncle Bob, adaptadas a este proyecto.
En cada cambio: **primero que funcione, luego dejarlo bien, y seguir la práctica.**

## Primero que funcione

- **Cada decisión en un solo sitio.** Un umbral, un fichero de ajustes, un protocolo: una constante o una función.
  Ejemplos: `appSettings()` (src/app_settings.h), `kOnAirS` / `onAir()`, `SurveySession`. Si algo se repite, se une.
- **El cambio más pequeño que resuelve el problema.** Nada de arreglos por si acaso.
- **TDD.** Antes de cambiar comportamiento, la prueba que falla; verla fallar por la razón esperada (si falla al
  enlazar porque la función no existe, vale); luego el cambio mínimo que la hace pasar. Si una prueba honesta no es
  práctica (solo con el adaptador), no se escribe una mala: se dice en el commit.
- **Modelar el dominio para que lo ilegal no se pueda escribir.** Un `enum class` en lugar de varias banderas que
  admiten combinaciones imposibles (`StepPhase`, `SurveyPass` en src/radio/radio.h); el estado de una sesión en un
  `struct` que se reinicia entero (`run_ = Run{}`), no campo a campo.
- **Los datos se comprueban donde entran; dentro, se confía en ellos.** El JSON de la radio se lee en
  `radio::parseStatus` / `parsePilots` (src/radio_status.cpp) y dentro solo circulan `RadioStatus` y `Pilot`.
- **La lógica, en funciones puras, fuera de los widgets.** Recomendador (`advisor::`), carrera (`race::`), ELRS
  (`elrs::`), parámetros de vídeo (`VideoEncoder::paramsFor`): sin Qt Widgets, con sus pruebas. Las ventanas solo
  muestran y emiten órdenes.
- **Estado compartido entre hilos:** si no hace falta compartirlo, cada hilo tiene el suyo (el estado del paso del
  escaneo es del hilo DSP); si hace falta, un escritor y lo que se lee de `_m`, de una vez y con un solo bloqueo.
- **No silenciar al compilador.** Un `-Wno-...` solo con su motivo escrito al lado (CMakeLists.txt, MinGW y `%zu`).

## Luego dejarlo bien

- **Funciones cortas y con pocas ramas.** Orientativo: menos de ~60 líneas y ~20 decisiones. Se parten en pasos
  con nombre (`elrs::analyse` → `find_wifi`, `measure_channels`...). Las ventanas, por bloques
  (`buildSide`, `buildRacePage`...). Los `paintEvent`, por capas con una `Geometry` (`drawAxes`, `drawSpectrum`...).
  Las listas planas de textos (`retranslate`, `tipText`) se dejan si se leen bien.
- **Corregir la causa, no tapar el síntoma.** Reproducirlo primero (mejor con una prueba).
- **Probar el comportamiento, no la implementación.** La prueba usa el código como su usuario y mira lo que él vería.
  Las ventanas se prueban por sus controles con `test/analyzer_driver.h` (por `objectName`), no con métodos
  `*ForTest`. Si hace falta un control nuevo en una prueba, se le da `objectName`. Las únicas costuras aceptadas son
  lo que no se ve en los controles (el reloj del análisis, `setTimeForTest`), y van documentadas.
- **Refactorizar:** fijar lo que hace (si no hay prueba, escribirla y comprobar que pasa con el código de antes),
  cambiar la estructura y demostrar que no ha cambiado: el mismo número de comprobaciones en `run-tests.sh` antes y
  después, y las mismas capturas.
- `crapper`, `dryer` y `mutator` (las herramientas de ub-mode) no admiten C++: la complejidad y la duplicación se
  revisan a mano.

## Seguir la práctica

- **Commits pequeños y en orden**, uno por cambio que se pueda comprobar solo. Mensaje en español:
  título corto, y en el cuerpo *Por que*, *Que* y *Como se ha probado*.
- **Pruebas con hardware (adaptador o VTX): avisar antes y esperar confirmación.** Nunca lanzar `radio_live` ni la app
  con el adaptador sin preguntar. Sin hardware se prueba con `radio_sim` y grabaciones sintéticas.
- **No ejecutar la app real para comprobar algo**: escribe en el `.ini` y el log del usuario. Usar las capturas de
  `test/` (cada una con su propio `.ini` temporal).
- **PRs**: título y descripción en inglés y en español (inglés primero, luego `## Español`), sin la línea
  "Generated with Claude Code".

## Compilar y probar (Windows, consola MSYS2 UCRT64)

```sh
scripts/build.sh                      # build/msys2 (Linux: build/release)
scripts/run-tests.sh                  # pruebas sin adaptador (las mismas que la CI)
scripts/radio-sim-test.sh             # fpv::Radio con FM y HDZero sintéticos en radio_sim (también en la CI)
```

Fuera de la consola de MSYS2, con `MSYSTEM=UCRT64` y `TMP`/`TEMP` apuntando a una carpeta escribible (si no, gcc
falla con "Cannot create temporary file in C:\Windows\").

Capturas para revisar ventanas (`QT_QPA_PLATFORM=offscreen`), compilar antes su objetivo:

```sh
build/msys2/analyzer_shot.exe salida.png 4 1 8 [--pin R1,R3] [--race] [--history] [--exclude R2]
build/msys2/elrs_shot.exe salida.png 6
build/msys2/main_window_shot.exe salida.png 6
```

Una prueba nueva se añade a `CMakeLists.txt` (`EXCLUDE_FROM_ALL`, con `check(...)` y salida 1 si falla) y a la lista
`TESTS` de `scripts/run-tests.sh`.

## Estilo

- Comentarios en español, sin tildes (salvo en `src/radio` y `src/kp_ext`, en inglés como kepyur). Explican el
  porqué, con la medida real cuando la hay ("medido: ...").
- Textos de la interfaz en inglés en el código (`tr()`); el español va en `translations/FPVDesktopWiFiReceiver_es.ts`.
  Tras cambiar textos: `cmake --build build/msys2 --target update_translations`. Si se mueve código con `tr()` a
  otra clase, el contexto cambia: copiar antes los mensajes del contexto viejo al nuevo en el `.ts` y pasar
  `update_translations`, para no perder traducciones (0 `unfinished` al acabar).
- Líneas de hasta 120 columnas en lo nuevo.

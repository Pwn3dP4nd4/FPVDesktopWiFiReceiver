# Analizador de canales

*[English](Channel-analyzer) · **Español***

El analizador (*Herramientas → Analizador de canales*) barre la banda de 5,8 GHz con el adaptador y recomienda
qué canales usar para varios pilotos analógicos y HDZero a la vez, de forma que no se pisen entre sí ni con lo que
ya hay en el aire.

> Esta página explica **en qué se basa** la recomendación. Los pasos para usar la ventana están en el
> [README](https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver/blob/main/README.es.md#analizador-de-canales).

## Qué mide

Con ganancia fija, por cada 156 kHz de la banda guarda tres cosas, como media de los últimos barridos
(**Recordar**: 5, 10, 20 o 40):

- **Nivel medio** sobre el ruido: lo que está siempre (un VTX, un router muy usado).
- **Ráfaga más fuerte reciente**, que se va olvidando: delata lo que aparece a ratos.
- **Parte del tiempo encendido**: cuánto pisa ese trozo de banda algo intermitente.

## Cómo elige

Cada canal candidato se puntúa, y gana el que menos suma:

1. **Interferencia media** dentro de su ancho (analógico 20 MHz, HDZero 27 MHz o 17 en modo estrecho).
2. **Ráfagas**: la parte del tiempo que lo pisa algo intermitente (Wi-Fi y similares). La media sola las esconde.
3. **Redes Wi-Fi vistas**, aunque ahora estén calladas (ver abajo).
4. Un canal donde **ya vuela un piloto** queda descartado. Un piloto en un canal fijado ya es tuyo (ver abajo);
   cualquier otro VTX tuyo se puede marcar en *En el aire* para que no cuente.

Entre los canales elegidos exige una separación mínima, penaliza quedar por debajo de la ideal, evita la
intermodulación de 3.er orden entre analógicos y, con la banda limpia, los separa todo lo posible.

## Redes Wi-Fi

El Wi-Fi de 5 GHz es la interferencia más habitual en 5,8 GHz, y tiene una trampa: **un router sin tráfico casi
no se ve**. Solo emite un *beacon* (un paquete corto) unas diez veces por segundo, así que apenas pisa el canal y
su media es casi la del ruido. En cuanto alguien usa esa red (un vídeo, una descarga), el canal queda inservible.
Por eso el analizador no solo mira cuánto ocupa cada canal ahora, también **qué redes hay**:

- **Cómo las reconoce**: un canal de la rejilla Wi-Fi (5000 + 5·n MHz) que se enciende a ráfagas en casi todo
  su ancho (~17 MHz), claramente por encima de los picos del ruido. Una ráfaga estrecha, un piloto (que emite sin
  parar) o lo que derrama una señal fuerte a sus lados no cuentan.
- **Las recuerda 5 minutos**: cada paso del barrido mide unos milisegundos y un router callado se pilla a ratos.
  Lo que se ha visto una vez se sigue teniendo en cuenta hasta 5 minutos después, aunque no salga en cada
  barrido. Al empezar un análisis nuevo se olvida todo.
- **Cuánto pesan**: según lo fuerte que llega la red, hasta 6 dB en los canales que pisa.
- **Su ancho**: un router a 40 u 80 MHz emite los beacons solo en su canal principal; el resto de su bloque solo
  se enciende con tráfico (comprobado con el adaptador). Mientras no se le ha visto tráfico, su bloque entero
  cuenta la mitad. En cuanto se le ve tráfico, se sabe qué usa y deja de contar lo que no.
- **Reglas Wi-Fi** (Europa / EE. UU. y otros): en Europa no existen los canales 144 ni 169–177, así que un router
  en el 132–140 llega como mucho a 40 MHz y uno en el 165 va solo. Por defecto se elige según la configuración
  regional del ordenador.

En la gráfica salen como **WiFi 149** (o **WiFi 100–112** si es una red de 80 MHz), con su bloque en discontinua
mientras no se sabe su ancho.

## Canales fijados y seguimiento de carrera

**Fijar canales.** Cuando ya has repartido canales a algunos pilotos, fíjalos: marca la casilla **Fijar** de su
fila en la tabla de la recomendación, o haz clic derecho sobre una marca de canal de la gráfica para fijar
cualquier otro (también uno que no esté recomendado). Los fijados entran siempre en la recomendación, aunque estén
ocupados, sucios o fuera de las bandas elegidas, y la búsqueda solo elige los que faltan, respetando con ellos la
separación y la intermodulación: con 4 pilotos y 2 fijados, busca 2. En la gráfica se ven con la franja más
intensa, los bordes marcados y una chincheta; en el resumen, *Fijados* aparte de *Sugeridos para los demás*. Se
guardan y se conservan entre análisis y sesiones. **Quitar todos los fijados** los borra.

Un piloto en el aire en un canal fijado (mismo sistema y a menos de 5 MHz) es siempre tuyo: no ocupa su propio canal
ni cuenta como interferencia, y en la lista *En el aire* sale marcado y en gris, *(fijado)*. La casilla de esa lista
es para tus otros pilotos, los que aún no están fijados: su canal no se toma como ocupado y la recomendación puede
darles otro, o poner a alguien a su lado, como si también se fueran a mover. Si uno ya está en un canal
recomendado, su fila dice *ya está aquí tu piloto …*; si no, el resumen avisa de que tiene que cambiar de canal. Si
se va a quedar donde está, fija su canal en vez de marcarlo. La marca vale para un análisis.

**Seguimiento de carrera** (pestaña *Carrera*). Con canales fijados y el análisis en marcha, marca **Seguimiento
de carrera** cuando empiece la carrera: el analizador sigue barriendo y, barrido a barrido, apunta para cada canal
fijado:

- la **SNR** de su piloto (mediana y los peores momentos, percentil 10);
- los **cortes**: barridos en que no se le ve, entre la primera y la última vez que se le vio (el despegue y el
  aterrizaje no cuentan);
- los **intrusos**: parte de los barridos en que una señal ajena (que no casa con ningún fijado) se le mete encima;
- la **interferencia** de su canal, con la señal de su propio piloto quitada (ruido, ráfagas, Wi-Fi recordado);
- una **calidad** estimada del vídeo (la peor SNR menos lo que quitan el canal y los cortes). El peor se resalta.

Durante los primeros 10 barridos está aprendiendo. Después, si a uno
de los pilotos con peor vídeo le iría claramente mejor (al menos 3 dB) en otro canal libre compatible con los demás
fijados, lo recomienda («Pasa al piloto de R3 a R2: unos +X dB», con la causa: otro emitiendo encima, o
interferencia o Wi-Fi en su canal). **Aplicar** cambia ese fijado (dile al piloto que cambie de canal) y vuelve a
empezar solo su seguimiento; los demás conservan lo apuntado. Cambiar los fijados a mano reinicia el seguimiento.

Lo que no puede ver:

- **La interferencia que cae justo debajo de la portadora de un piloto que está emitiendo**: se quita con su
  señal. Se ve en los barridos en que ese piloto no está, en las redes Wi-Fi recordadas y en lo que asoma fuera de
  su ancho.
- **Una SNR baja o cortes con el canal limpio no se arreglan cambiando de canal**: son la antena, la potencia del
  VTX o la distancia. En ese caso no recomienda mover y lo dice.

## Ideas tomadas del vídeo de Ryan Quellet

Parte de esto parte del vídeo
[*FPV Range Secrets Revealed – Analog, HDZero, Divimath*](https://youtu.be/Yqq9Sd2aWXg) (Ryan Quellet,
DiviMath/HDZero), que da consejos para elegir canal y alcance. Qué ideas se han usado y cómo:

| Idea del vídeo | Minuto | En el analizador |
|---|---|---|
| Medir el espectro con un analizador Wi-Fi y elegir canal según lo que hay | [14:55](https://youtu.be/Yqq9Sd2aWXg?t=895) | Es lo que hace el analizador, pero midiendo la banda FPV con el propio adaptador en lugar de un móvil |
| Evitar los canales que coinciden con el Wi-Fi de 5 GHz habitual (R3–R6) | [17:17](https://youtu.be/Yqq9Sd2aWXg?t=1037) | No como regla fija: se miden las redes que hay de verdad y se penalizan aunque estén calladas |
| Un router a 40/80 MHz ocupa varios canales | [14:55](https://youtu.be/Yqq9Sd2aWXg?t=895) | Se tiene en cuenta su bloque mientras no se sabe su ancho |
| Los canales DFS (R1, R2, Low Race) son "seguros" porque el router tiene que apartarse | [15:56](https://youtu.be/Yqq9Sd2aWXg?t=956) | **No se usa**: DFS obliga a apartarse ante un radar, no ante un VTX. Una red en un canal DFS cuenta igual |
| Modo estrecho de HDZero (17 MHz en vez de 27) para más alcance | [11:08](https://youtu.be/Yqq9Sd2aWXg?t=668) | Ya estaba: casilla *HDZero en modo estrecho* |
| La potencia del VTX no es igual en todos los canales (más en el centro) | [17:37](https://youtu.be/Yqq9Sd2aWXg?t=1057) | **No se usa**: no hay medidas fiables de cada VTX |
| Una emisora ELRS/Crossfire potente junto al receptor lo satura | [11:48](https://youtu.be/Yqq9Sd2aWXg?t=708) | **No se detecta todavía**: sube el ruido de toda la banda por igual, y el analizador mide sobre el ruido |
| Low Race solo con autorización | [18:19](https://youtu.be/Yqq9Sd2aWXg?t=1099) | La banda L está desmarcada por defecto |

## Lo comprobado con el adaptador

Con un RTL8812BU en USB 3:

- Los picos del ruido no son despreciables (la mitad de la banda llega a ~6 dB y hay tramos enteros a 7–9 dB),
  así que el umbral para reconocer una red se ajusta al ruido de cada momento.
- Una red de 20 MHz con tráfico se reconoce en el primer barrido, con su forma característica (16,6 MHz con un
  hueco en el centro).
- Un router a 80 MHz con un vídeo enciende su bloque entero (80 MHz), no solo su canal principal.
- Una señal fuerte derrama a los lados (en el caso medido, de 24 dB a 13 dB en los 25 MHz siguientes); eso no se
  toma por otra red.

## Limitaciones

- **Lo que ve el analizador está donde está el adaptador.** Si las gafas o el receptor están en otro sitio, lo
  que reciben puede ser distinto.
- **Un router que no se ha pillado nunca no cuenta.** Deja el análisis en marcha al menos un minuto antes de
  fiarte de la recomendación.
- **El analizador barre de 5300 a 6000 MHz.** Las redes en los canales 36–64 de 5 GHz quedan casi todas fuera,
  pero tampoco llegan a ningún canal FPV.
- **No detecta la saturación por una emisora potente cercana.** Si usas la radio de control a mucha potencia,
  aléjala del adaptador y de las gafas.

## Normativa

En la UE, el vídeo FPV de 5,8 GHz va en la banda de corto alcance **5725–5875 MHz con 25 mW PIRE**. R1, R2, R8 y
la banda L caen fuera. Comprueba lo que se permite donde vuelas: el analizador recomienda entre las bandas que
marques, no comprueba si son legales.

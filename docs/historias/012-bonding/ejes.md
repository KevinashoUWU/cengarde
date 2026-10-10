# Síntesis por eje del estudio de bonding (2026-10-10)

Ocho ejes, escritos por cuatro sintetizadores sobre las fichas ya supervisadas. Son anteriores a la auditoría: lo que la auditoría corrigió está aplicado en las matrices finales, no aquí. En particular, la síntesis de **reordenamiento** se armó sobre una premisa falsa del contexto (que cengarde deduplica por el contador de WireGuard); el código usa una secuencia propia de 32 bits bajo el MAC, y la matriz de cengarde ya lo corrige.

---

## Reparto y planificación entre enlaces (planificadores, WRR por capacidad, k-de-N, duplicación selectiva)

## Eje 1: reparto y planificación entre enlaces

### 1. Lo que fijan las normas: dos modos y nada más
Ninguna norma define planificador, pesos, k-de-N ni duplicación selectiva. Eso queda como política local [F2-rfc8684-mptcp A1], [F2-mp-dccp A1, A10], [F1-vsf-tr06-2-main A9]. El borrador de QUIC multipath dice que no existe ninguna especificación IETF de planificación concurrente [F2-quic-multipath N1]. Lo que sí fijan son dos separaciones:
- replicar (estilo ST 2022-7) frente a repartir para sumar capacidad [F1-vsf-tr06-2-main A1, A2], [F1-vsf-tr06-1-simple A1, A2];
- reparto por flujo frente a reparto por paquete [F2-bbf-tr348 A3].

Solo el reparto por paquete deja que un único flujo use la suma de los enlaces [F2-rfc8157-gre-bonding A6]. Es lo que necesita la Fase 5, porque cengarde no ve los flujos.

### 2. Duplicar todo tiene un techo conocido
Cada enlace extra cuesta 100 % más y solo una copia aporta datos útiles [F1-srt-socket-groups A2]. La velocidad queda en la del mejor enlace [F6-speedify-bonding-mode A3]; la fuente es un vendedor, pero es aritmética. Los productos comerciales tratan la duplicación como modo de excepción [F6-speedify-redundant-mode A5].

En cengarde hay además un costo que las fuentes no miran. Cada datagrama son N envíos en una Pi que ya está limitada por CPU. Repartir no solo suma capacidad de enlace: también divide por N los envíos por paquete [razonado].

### 3. minRTT: bien con colas chicas, mal con bufferbloat y picos de radio
El planificador por defecto de MPTCP manda cada paquete por el subflujo de menor RTT que tenga hueco en su ventana de congestión [F3-raiciu-nsdi12 A9], [F3-mptcp-sched-eval-2025 A1]. La evidencia está partida.

**En contra:**
- Con caminos heterogéneos subutiliza el rápido. Solo lo dice el resumen de ECF [F3-ecf A2].
- En el banco de motivación de QAware usó ~60 % del agregado (dos subflujos, último tramo WiFi 802.11g) [F3-qaware].
- En la tesis de Cech, con anchos de banda distintos y colas grandes en el router (1000 paquetes), se queda solo con la ruta sin degradar [F3-cech-thesis A1, A5].
- En dual-LTE un pico de RTT (3× tras una caída de señal, hasta 10× tras un handover) hace que minSRTT desvíe todo al otro operador para no desordenar. Eso desbalancea los subflujos y el throughput cae 65 % en movilidad baja [F8-dual-lte-mptcp A2, A1 corregida].
- Meta-DAMS le saca ~30 % con RTT parecidos y ~40 % cuando la diferencia de RTT crece. Es Mininet con descargas de archivos y dos caminos [F7-meta-dams A7, N5].

**A favor:**
- Con colas chicas (300 paquetes) minRTT sí reparte, mientras RR, BLEST y ECF usan casi un solo camino [F3-cech-thesis A6].
- En el preprint de 2025 (Mininet, 2×100 Mbit/s, bulk), con BBR, C-MPBBR u OLIA, minRTT y ECF dan el mayor goodput y el menor retardo en casi todos los escenarios [F3-mptcp-sched-eval-2025 A5].
- En el banco real de QAware (dos enlaces Gigabit, retardos <1 ms) minSRTT superó a DAPS, BLEST y ECF [F3-qaware N2].

Lectura que reconcilia ambos lados [razonado]: minRTT funciona cuando el RTT refleja pronto la carga, o sea con colas cortas. Con bufferbloat el RTT suavizado llega tarde y el planificador carga de más el enlace que parece mejor. Esta explicación es de los autores de QAware, no una medida [F3-qaware A3]. 5G y Starlink son justo el caso de colas grandes.

En cengarde, además, no hay cwnd por enlace. Un minRTT sin tope mandaría todo al enlace de menor OWD. Para sumar capacidad necesita un tope de cola o de capacidad, y con ese tope se convierte en el planificador por llegada esperada de la sección 5 [razonado].

### 4. Esperar al rápido: BLEST y ECF
BLEST estima cuánto enviaría el subflujo rápido durante el RTT del lento. Si eso no cabe en la ventana, no usa el lento y espera [F3-blest A1].

**Resultados a favor:**
- En emulación 3G+WLAN bajó 19 % el buffer fuera de orden (OFO) y subió 12 % el goodput frente a minRTT. Condiciones: WLAN 25 Mbit/s, 25 ms, 1 % de pérdida; 3G 5 Mbit/s, 65 ms; tráfico bulk [F3-blest N1].
- El kernel Linux adoptó en 2021 una versión simplificada: transmite solo por el subflujo con menor tiempo estimado de vaciado según su pacing rate, y espera si su ventana está cerrada [F3-blest N2].
- OpenMPTCProuter usaba BLEST por defecto con el MPTCP fuera de árbol, en kernel 5.4 [F5-openmptcprouter N1].

**El costo es capacidad:**
- Con tres caminos Gigabit, BLEST y ECF usan 62 % del ancho de banda frente a 82 % de RR, MuSher y STTF [F3-cech-thesis A4].
- Con heterogeneidad fuerte, BLEST deja de usar la vía lenta y queda entre los últimos [F3-mptcp-sched-eval-2025 A4].
- En Cech gana con retardos heterogéneos justamente porque usa un solo flujo [F3-cech-thesis N2].

Los autores del preprint de 2025 concluyen que el mejor planificador depende de la aplicación. También conjeturan, sin medirlo, que BLEST sería mejor si importa la suavidad y no el throughput [F3-mptcp-sched-eval-2025 N1, A6].

De ECF solo se leyó el resumen, sin cifras [F3-ecf A4, A5 matizadas]. Cech no reproduce su ventaja con cuatro caminos Gigabit [F3-cech-thesis N1], y QAware lo pone por debajo de minSRTT [F3-qaware N2], aunque con kernels MPTCP distintos (v0.89 frente a v0.93).

srtla-send-rs, un proyecto de bonding real, probó y eliminó un pipeline completo de predicción de llegada con EDPF, BLEST e IoDS, sin publicar cifras [F5-srtla-send-rs N1]. Es una señal, no evidencia.

### 5. En lo que coinciden varias fuentes: planificar por llegada esperada
Varias fuentes independientes llegan al mismo criterio: mandar cada paquete por el enlace que lo entregaría antes.
- QAware elige el subflujo que minimiza (n_k+1)·Ŝ_k, con n_k la cola local del driver y Ŝ_k el tiempo de servicio estimado [F3-qaware A2]. En banco real (dos enlaces Gigabit, retardos por defecto, bulk) dio +37 % sobre minSRTT y más de 45 % sobre DAPS, BLEST y ECF [F3-qaware N2]. Las cifras de simulación con retardos 40/80 ms se refutaron y no se usan.
- Meta-DAMS reparte al camino de entrega más temprana para minimizar el costo de reordenar [F7-meta-dams A2].
- FMTCP asigna por tiempo de llegada esperado; solo se leyó el resumen [F4-fmtcp A3].
- El trabajo con realimentación retardada propone EDF cuando los retardos difieren, sin evaluarlo [F4-joint-sched-coding-delayed-feedback A6].
- El BLEST del kernel es una versión de lo mismo [F3-blest N2].

**Traducido a cengarde [razonado].** ETA_i = OWD_i + (bytes en la cola del enlace i + len)/C_i, y se elige el mínimo. Basta la OWD relativa entre enlaces, porque el desfase de reloj se cancela al comparar. Todo sale de datos que la regla de cengarde permite: la OWD de las sondas autenticadas, C_i estimada con las estadísticas de primeras copias, y la profundidad del anillo del hilo de cada enlace (diseño A.7).

Tiene tres propiedades:
- (a) Con poca carga todo va por el enlace de menor OWD, que es la latencia mínima posible.
- (b) Cuando la cola del rápido alcanza la diferencia de OWD con el siguiente, desborda a él. Así se suma capacidad solo cuando hace falta.
- (c) Si las estimaciones aciertan, los paquetes llegan casi en orden, y el reordenador del receptor solo absorbe los errores.

En saturación, la latencia del agregado sube hasta la OWD del enlace más lento que se esté usando. Ningún planificador lo evita: es la regla de que el agregado hereda la peor latencia [F5-mlvpn A4], [F2-quic-multipath A5].

Condición dura: la cola tiene que vivir en cengarde (pacing por enlace a C_i en el hilo de cada enlace) y no en el módem. Si C_i se sobrestima, la cola se va al CPE, la ETA miente y vuelve el problema de minRTT con bufferbloat [razonado][MEDIR]. Cech mostró que el tamaño de las colas intermedias invierte el ranking de planificadores [F3-cech-thesis A1, A2].

### 6. WRR por capacidad y puntajes de ventana
**WRR por capacidad.** Es lo que hacen:
- MLVPN: peso = ancho de banda configurado; si un enlace no lo tiene, todos pesan 1 [F5-mlvpn A5, A6];
- el BPF bpf_weight_rr de OpenMPTCProuter [F5-openmptcprouter A2];
- libRIST.

Lo prevén TR-348 y ATSSS [F2-bbf-tr348 A1], [F2-3gpp-ts24193-atsss A9]. Ninguna fuente mide WRR sobre enlaces de radio.

Con proporciones fijas, cada enlace con peso > 0 lleva tráfico también con poca carga. Con reordenador, todo el tráfico paga entonces la OWD del enlace más lento que tenga peso [razonado; F2-quic-multipath A5]. Peplink (vendedor) recomienda enlaces dentro de 50 % de ancho y 150 ms de latencia entre sí. Si la latencia varía mucho, aconseja dedicar al túnel los de baja latencia y dejar los demás de respaldo [F6-peplink-speedfusion-whitepaper N3, N6].

**Puntaje de ventana (srtla).** Elige por paquete el enlace con mayor ventana/(en vuelo+1), con la ventana movida por ACK y NAK [F5-belabox-srtla A1]. Es barato y está en producción, pero tiene tres problemas:
- es ciego a la latencia y confunde desorden con pérdida: un NAK por un paquete que solo venía lento resta ventana a ese enlace [A4];
- el autor avisa que un lossmaxttl chico manda casi todo por un enlace e impide agregar [N1];
- no excluye enlaces lentos (el código está comentado) [A10].

srtla-send-rs añade una penalización de 0,7 a los enlaces con ráfagas de NAK y no compuerta un enlace si no queda otro sano [F5-srtla-send-rs A12, A4]. Lo transferible a cengarde [razonado]: la realimentación de entrega por enlace (cabe como tipo de mensaje nuevo en v4) y la regla de no bajar la capacidad estimada por un desorden.

### 7. Aleatorio ponderado, flowlets y prioridad
**Aleatorio ponderado.** HighRB elige camino al azar con peso = bytes libres de la ventana, lo que además sondea los caminos no preferidos [F4-quic-fec-mpquic A8, A9]. Round-robin solo conviene con caminos parecidos [A7]. Esa heterogeneidad se probó solo en pérdida, con retardos similares [N2].

**Flowlets (LetFlow).** Cambiar de camino entre ráfagas no desordena si el hueco entre ellas supera la diferencia de latencia [F9-letflow A1]. En datacenter queda a 2× del óptimo para pesos de 20 a 95 % [A2]. Un túnel que agrega muchos flujos no tiene huecos de 100 ms, así que no hay flowlets [razonado].

**Prioridad y costo.** Hay varios mecanismos, ninguno con medición:
- el bit B de MPTCP [F2-rfc8684-mptcp A2];
- MP_PRIO secundario para caminos con costo o cupo de datos [F2-mp-dccp A2, A3];
- menor costo primero con desborde [F2-bbf-tr348 A2];
- el modo priority-based de ATSSS [F2-3gpp-ts24193-atsss A2];
- LiveU (vendedor) y Dejero (patente) priorizan por red y costo [F7-liveu-lrt A3, A4], [F7-dejero-us10028163 A1, A2].

Solo sirve si algún enlace tiene cupo de datos.

### 8. Cuándo duplicar y cuándo repartir
Ninguna fuente primaria mide k-de-N. Lo que hay:
- Peplink WAN Smoothing duplica hacia otro enlace con latencia más estable, con topes declarados de 2×, 3× y 4× de consumo [F6-peplink-speedfusion-whitepaper A1, A2]. Según la corrección del supervisor también duplica los ACK TCP dentro del túnel. Es vendedor, sin datos.
- ATSSS redundant: con acceso primario puede duplicar en el otro, «implementation dependent». Con umbral de pérdida, duplica solo si ambos accesos lo exceden; si no, usa el sano o el primario [F2-3gpp-ts24193-atsss A4]. Solo está en el espejo Rel-19, no en la V17.9.0, y no vale para ATSSS-LL [N1].
- Speedify conmuta los flujos de streaming entre bonding y redundancia «según lo que funcione mejor», sin publicar el criterio [F6-speedify-bonding-mode A5], [F6-speedify-streaming-mode A1, A3].
- TR-348 saca la telefonía del reparto por paquete y la manda por un solo camino, porque el retardo de reorden degrada la experiencia [F2-bbf-tr348 N1].
- La única medición de producción es XLINK. Reinyectar sin control sumó 15 % de tráfico. Reinyectar solo cuando el buffer de reproducción baja de los umbrales (95, 80) costó 2,1 % y bajó el rebuffering entre 23 y 67 % frente a QUIC de un camino, en un A/B de Taobao con más de 100 mil participantes [F3-xlink A1, A2]. El 66 % de la Tabla 2 es un proxy del rebuffering [A3]. El multicamino sin control fue peor que un solo camino [F3-xlink, A8 corregida].
- Un respaldo que no puede llevar el bloque entero a tiempo sirve poco; es un resultado de modelo analítico [F4-joint-sched-coding-parallel A4].

Lo que se sigue [razonado]:
1. Duplicar un flujo completo en k enlaces limita su tasa a la del k-ésimo enlace. Por eso k-de-N para todo el tráfico choca con la meta de velocidad.
2. Duplicar es casi gratis en capacidad mientras la carga ofrecida está por debajo de la capacidad de los k enlaces elegidos. Su costo real en cengarde es CPU (k envíos por paquete) y bytes.
3. cengarde ve el tamaño y el tipo de mensaje WireGuard. Los candidatos obvios a k = 2 son los handshakes (tipos 1 a 3), los keepalives y los datagramas chicos: un ACK TCP puro ocupa ~100 B de carga WireGuard, y VoIP, DNS y los ACK de QUIC también son chicos. Los grandes van por ETA.
4. El umbral y el costo son [MEDIR]. Un ACK puro cuesta un paquete entero de CPU aunque sean pocos bytes, y duplicarlo por los 5 enlaces multiplica los envíos de subida.
5. El disparador estilo XLINK que cengarde puede ver es la degradación de OWD o de pérdida en las sondas, como los umbrales de ATSSS.
6. El resumen F8 recoge que, en dos operadores medidos en un coche, la pérdida conjunta fue 3,5× la de la independencia. Eso sugiere elegir los k enlaces por diversidad de medio (Starlink + el mejor 5G) antes que por «los k mejores» [MEDIR en Santiago].

### 9. Lo que no cabe en una Pi
El reparto por programa lineal, con reintento por el camino de menor latencia [F4-deadline-aware-multipath A4], tarda ~458 µs por resolución con dos caminos en un i5 de 2,8 GHz y crece de forma exponencial con los reintentos [A5]. Meta-DAMS (A3C con LSTM, 729 metamodelos para pérdida <1 % y RTT ≤200 ms) no mide el costo de decidir [F7-meta-dams A4, A5, A6]. Ninguno cabe por paquete en la Pi. De ellos sirve la regla, no el método.

### 10. Fallos de campo
- OpenMPTCProuter en una RPi4 con LTE + Starlink: el agregado medido quedó por debajo del enlace más lento. El mantenedor reconoce el problema y sugiere quitar SQM y probar Cubic, sin dar causa [F5-openmptcprouter, A8 corregida].
- XLINK: el multicamino sin control fue peor que un camino en tiempo de completado, y el rebuffering empeoró entre 34 % y 96 % [F3-xlink, A8 corregida].
- srtla: un lossmaxttl chico mata la agregación [F5-belabox-srtla N1].
- srtla-send-rs: la primera exploración desviaba cerca de la mitad del tráfico. Su versión acotada no tuvo efecto medible en netem (t de Welch 0,75, n = 15) y se eliminó [F5-srtla-send-rs, A9 corregida].
- Dual-LTE: minSRTT desbalancea tras los picos de RTT [F8-dual-lte-mptcp A2].

### 11. Qué significa
**cengarde [razonado].** cg_sched_mask(links, n, len, now) puede componerse en tres pasos puros y testeables:
1. Clase por tamaño y tipo WireGuard: el control y los chicos van a una máscara de 2 enlaces (menor OWD, medios distintos).
2. Los grandes van al mínimo de ETA entre los enlaces no silenciados, con tope de cola por enlace y pacing en el hilo de cada enlace.
3. Con carga baja y CPU libre se puede seguir duplicando todo como hoy. Cuando la carga se acerca a la capacidad del mejor enlace, se pasa a repartir los grandes, con histéresis.

El silenciado de la historia 006 ya acota el spread entre enlaces con datos (~150 ms). Todo usa OWD de sondas, capacidad desde primeras copias y profundidad de anillo, nunca datos sin MAC.

**SmoothStream.** Nada aquí reabre el WRR de libRIST, el peso 0 para audio ni el minimax de paridad. Lo que aporta a T7:
- el patrón XLINK: subir redundancia solo cuando el margen del buffer se acorta, con dos umbrales que dan histéresis [F3-xlink A4, A5];
- la prioridad por tipo de cuadro (I-frames) [F7-meta-dams A1], como idea a medir;
- que el round-robin de paridad solo conviene con patas parecidas [F4-quic-fec-mpquic A7], coherente con la regla spread/plazo ya decidida.

El WRR encaja con RIST porque su receptor ya fija la latencia, así que el reparto proporcional no agrega espera [razonado].

**Qué medir:**

- Spread de OWD entre los 5 enlaces (WOM, Entel, Claro, Movistar, Starlink) en ambos sentidos, en reposo y bajo carga: p50, p95 y p99 por hora. Cuánto sube la OWD con carga en cada enlace (cola del CPE o del módem).
- Capacidad sostenida por enlace y por sentido, y cómo varía en el tiempo, incluidos los bordes de 15 s de Starlink y los handovers 5G. Cuánto se equivoca una C_i estimada con primeras copias.
- Banco con netem y trazas reales, y después campo. Comparar duplicación total (hoy), WRR por capacidad estimada, ETA (OWD + cola/C_i) y minRTT con tope. Métricas: goodput de 1 y de N flujos TCP Cubic y BBR (Linux, Android, Windows, iOS) y QUIC; latencia bajo carga; retransmisiones espurias interiores; ocupación del reordenador; CPU por paquete en la Pi.
- Objetivo del ROADMAP ≥1,8× con 2 enlaces iguales: medirlo con un flujo y con varios, con Cubic y con BBR, con y sin reordenador.
- Histograma de tamaños y tipos de mensaje WireGuard en tráfico real, de subida y de bajada: qué fracción de paquetes y de bytes queda bajo cada umbral candidato para la duplicación selectiva.
- Costo de CPU en la Pi de duplicar los paquetes chicos con k = 2 frente a k = N (envíos extra por segundo), y costo de la ETA por paquete (debería ser despreciable).
- Correlación de pérdidas y de picos de OWD entre los 4 operadores y Starlink en Santiago, para elegir los k enlaces de la duplicación por diversidad de medio.
- Umbral de carga (fracción de la capacidad del mejor enlace y de la CPU) para pasar de duplicar todo a repartir los grandes, con histéresis.
- Si el pacing por enlace a C_i mantiene la cola dentro del anillo de cengarde y no en el módem: comparar la OWD con y sin pacing al 80–100 % de la capacidad.
- Tiempo de reacción de la ETA ante un pico de OWD de 5G (3× a 10× según dual-LTE) con sondas cada 100 ms, y cuántos paquetes quedan en vuelo por el enlace que se degrada.
- SmoothStream (T7): margen del buffer RIST en el momento en que conviene subir redundancia (patrón XLINK) y dos umbrales con histéresis, medidos en fierro. Capacidad por pata para los pesos con la celda saturada.

---

## Reordenamiento en el receptor frente a lo que toleran TCP, QUIC y RTP interiores

## Eje 2: reordenamiento en el receptor frente a TCP, QUIC y RTP interiores

### 1. Repartir por paquete obliga a reordenar en el extremo
Todas las fuentes que reparten por paquete ponen en el receptor un reordenador con un número de secuencia común a todos los caminos:
- GRE: un único número para todos los túneles y un solo buffer [F2-rfc8157-gre-bonding A1, A2];
- MPTCP: el DSN, con buffer y ventana compartidos [F2-rfc8684-mptcp A4];
- MP-DCCP: MP_SEQ de 48 bits [F2-mp-dccp A7];
- RIST Main: S=1, recomendado y no obligatorio [F1-vsf-tr06-2-main A3, A4];
- MLVPN: un data_seq global [F5-mlvpn A1];
- SpeedFusion: una cabecera propia (vendedor) [F6-peplink-speedfusion-whitepaper N1].

TR-348 lo dice por la negativa: sin reordenador en el lado de red, el reparto por paquete no es posible [F2-bbf-tr348 A4, A5].

En cengarde ese número ya existe: el contador u64 de WireGuard, denso y en claro [razonado]. Hay detalles que las fuentes no cubren [razonado]:
- hay una secuencia por índice receptor;
- en cada rekey de WireGuard (≈ cada 2 minutos) conviven la sesión vieja y la nueva, así que hace falta un anillo por índice;
- los mensajes de handshake no llevan contador y pasan directo;
- falta verificar que el WireGuard emisor saque los contadores en orden [MEDIR].

### 2. Cuánto desorden tolera el tráfico interior
**TCP (Linux y Android).** RACK es el único detector de pérdida del kernel [F9-linux-ip-sysctl A4]. Declara perdido un segmento cuando uno enviado después ya llegó y pasó el RTT más la ventana de reorden [F9-rfc8985-rack-tlp N3]. La ventana:
- arranca en min_RTT/4 [A1], o en 0 si no se vio desorden y ya hubo 3 SACK [A2];
- crece (N+1)·min_RTT/4 por cada ronda con DSACK, hasta SRTT [A3, A4];
- vuelve al inicio tras 16 recuperaciones [A5].

Está diseñada para desorden dentro del RTT del camino más corto [N1]. Adaptarse cuesta varias rondas con retransmisiones espurias [N2]. Los sysctl tcp_reordering (3) y tcp_max_reordering (300) cuentan paquetes, y la propia doc sugiere subir el segundo con balanceo por paquete [F9-linux-ip-sysctl A1, A2]. De ahí no se puede concluir que 300 paquetes cubran decenas de ms de spread (corrección del supervisor).

**QUIC.** Declara pérdida si 3 paquetes posteriores ya fueron confirmados, o si pasaron más de 9/8·max(smoothed_rtt, latest_rtt) desde el envío, con piso de 1 ms [F9-rfc9002-quic-loss A1, A2]. Eso da ~1/8 de RTT de margen sobre el RTT, no 9/8 de diferencia entre enlaces (corrección del supervisor). Subir los umbrales tras pérdidas espurias es opcional [A5, N4]. El RFC espera más desorden en QUIC que en TCP, porque la red no puede reordenar paquetes cifrados [A6].

**Cuenta de ejemplo [razonado].** Con RTT interior de 40 ms, QUIC tolera ~5 ms de atraso extra, y RACK arranca en 10 ms y llega a 40 ms solo tras varias rondas con DSACK. Un spread de 100–150 ms entre operadores supera eso de lejos. Además, el umbral de 3 paquetes salta en cuanto tres paquetes del enlace rápido adelantan a uno del lento. Sin reordenador en el receptor, un TCP Cubic o un QUIC interior verían pérdidas falsas en ráfaga. Es la conclusión razonada del resumen F9; ninguna medición con tráfico real a través de bonding la confirma ni la contradice.

**DSACK** [F9-rfc2883-dsack A2, A3, A4] es lo que permite a RACK detectar sus retransmisiones espurias. Corrijo la aplicabilidad de esa ficha [razonado]. La dedup de cengarde descarta copias del mismo datagrama WireGuard, con el mismo contador. Una retransmisión del TCP interior es un datagrama nuevo con otro contador, así que el receptor interior sí ve el duplicado y manda DSACK. La adaptación de RACK funciona a través del túnel. Lo que cuesta es el tiempo y las bajadas de ventana mientras aprende, y se reinicia cada 16 recuperaciones.

**BBR interior.** Un detector tipo RACK puede marcar pérdidas mucho después de ocurridas, y el borrador corrige inflight_hi para ese caso [F9-bbr-draft A6]. No hay medida.

**Windows, iOS y macOS, y los QUIC de navegador:** nada en las fuentes [MEDIR].

**Tráfico no fiable (RTP, VoIP, juegos).** MP-DCCP dice, en un apéndice informativo, que muchas aplicaciones no fiables procesan datos fuera de orden y que el reordenamiento puede no hacer falta [F2-mp-dccp A6]. TR-348, en cambio, saca la telefonía del reparto por paquete justamente por el retardo de reorden [F2-bbf-tr348 N1, A6].

### 3. El tope duro: la ventana anti-replay de WireGuard
WireGuard guarda el mayor contador recibido y una ventana de contadores previos, que se consulta solo después de verificar la etiqueta [F9-wireguard-paper A1, A2, A6]. El algoritmo es el del apéndice C de RFC 2401 o el de RFC 6479; el paper no dice cuál ni el tamaño [A3].

Si la ventana es de 8192 contadores (dato del contexto, a confirmar en el código), en tiempo vale 8192/pps [razonado]:
- ~0,9 s a 100 Mbit/s con datagramas de ~1420 B;
- ~0,3 s a 300 Mbit/s;
- menos si dominan los paquetes chicos.

Lo que llegue con más atraso que eso es pérdida real, no desorden. Con un reordenador delante, la ventana solo importa para lo que cengarde suelte tarde.

### 4. Dimensionado: el spread fija el tiempo, la tasa fija la memoria
- RFC 8157 no recomienda un timer de reorden mayor que la diferencia normal de RTT entre los dos enlaces; el «e.g., 100 ms» es ambiguo [F2-rfc8157-gre-bonding A3]. Pide un buffer mayor que la suma de las tasas de línea por el timer [A4]. El «margen amplio» es afirmación del fabricante [N1].
- TR-06-1 (informativo): la sección de reorden debe cubrir el peor diferencial de retardo [F1-vsf-tr06-1-simple A6]. El tamaño depende de RTT, jitter y cortes [A8], y el default de 70 ms es solo una sugerencia [A11].
- RFC 8684: el buffer va entre el BDP máximo de un camino y RTT_max × ancho de banda total, y ni eso alcanza ante un RTO [F2-rfc8684-mptcp A5]. La relación con la retransmisión queda para estudio futuro [A6].
- Raiciu: Σx_i·RTT_max, doblado para seguir enviando durante una retransmisión rápida. Da 375 KB con 3G de 2 Mbit/s y 150 ms más WiFi de 8 Mbit/s y 20 ms [F3-raiciu-nsdi12 A1, N4]. Con buffer chico, una pérdida en el lento bloquea al rápido [A3]. Autoajustar midiendo el RTT de un camino con bufferbloat infla el buffer sin necesidad [A7].
- Peplink usa un jitter buffer de 150 ms por defecto y dice que subirlo «puede ayudar» si las latencias difieren mucho (vendedor) [F6-peplink-speedfusion-whitepaper A7].

**Para cengarde [razonado].** El factor 2 de Raiciu viene de la ARQ de MPTCP; cengarde no retransmite, así que la memoria es tasa × espera máxima. A 200 Mbit/s con 150 ms son ≈3,75 MB, unos 2700 datagramas de 1420 B. Caben en un anillo preasignado de 4096 entradas por índice, sin malloc por paquete. Como el contador es denso, el anillo se indexa por contador módulo N y la inserción es O(1). La búsqueda por lotes de Raiciu [A12] existe porque MPTCP ordena bytes.

La espera máxima ya está acotada por lo hecho. El silenciado de la historia 006 saca de los datos a un enlace que va más de 150 ms detrás del más rápido durante 2 s. Entre enlaces con datos el spread queda en ≤ ~150 ms, salvo transitorios de hasta 2 s.

### 5. Cuándo soltar un hueco: lo caro es la pérdida real
[razonado] Con pérdida p y tasa R hay p·R huecos por segundo. Si cada hueco retiene todo durante la espera máxima H, con 0,5 % de pérdida a ~8800 pps (100 Mbit/s) son ~44 huecos por segundo: el buffer pasa casi siempre reteniendo y todo el tráfico paga H.

Las fuentes ofrecen tres maneras de soltar:
- **Timer fijo** [F2-rfc8157-gre-bonding A3].
- **Timer de inactividad (MLVPN).** Lo calcula como 2,2 × máx(srtt + 4·rttvar) de los enlaces activos, o 0,8 s sin medición [F5-mlvpn A2]. Pero lo rearma con cada inserción, así que con tráfico continuo el hueco se rompe por desborde de ventana y no por tiempo [A3]. Al expirar vacía todo y resetea [A2]. Su ventana sugerida de 64 paquetes cubre unos 7 ms a 100 Mbit/s [razonado].
- **Diferencia de retardo entre caminos (MP-DCCP).** Da un hueco por perdido tras path_delta/2, solo con rutas simétricas y sin jitter [F2-mp-dccp A5].

**Propuesta [razonado].** Aplicar el criterio de RACK [F9-rfc8985-rack-tlp N3] por enlace. Si cada enlace entrega en orden (FIFO), el contador c está perdido cuando todos los enlaces que llevan datos ya entregaron algún contador mayor que c. Así la pérdida real se resuelve en cuanto el enlace más lento con datos muestra un paquete posterior.

Además, un tope por hueco: llegada del primer contador > c + (OWD máxima de los enlaces con datos − OWD del enlace por el que llegó) + margen. La OWD sale de las sondas autenticadas; es el path_delta sin el /2, porque cengarde mide la ida.

Con planificación por llegada (eje 1) los huecos se cierran en pocos ms; con WRR fijo, en hasta el spread. El supuesto de FIFO por enlace hay que medirlo, porque los cambios de ruta LEO reordenan dentro de un mismo camino [F8-leo-reordering A1, A3].

Al soltar por tiempo nunca se descarta: lo que llegue tarde se entrega igual y WireGuard decide. MLVPN también inyecta tal cual lo que cae fuera de su ventana [F5-mlvpn A1].

### 6. Bloqueo de cabeza de línea entre flujos
[razonado] El contador de WireGuard es uno solo para todo el tráfico interior. Un reordenador de túnel hace esperar a un paquete de VoIP o a un ACK detrás del hueco de una descarga ajena. QUIC multipath ya avisa que la entrega en orden de un stream sufre el retardo del camino más lento [F2-quic-multipath A5]; a nivel de túnel ese costo se reparte a todos los flujos.

Una salida coherente con el bypass de TR-348 [F2-bbf-tr348 N1] y con que el tráfico no fiable tolera desorden [F2-mp-dccp A6]: entregar sin esperar los datagramas chicos (los mismos que se duplican en el eje 1) y ordenar solo los grandes. La ventana de WireGuard acepta los adelantados. El costo y el efecto en QUIC, que también manda paquetes chicos de datos, son [MEDIR].

### 7. Seguridad del reordenador
TR-06-2 describe reordenar y deduplicar antes de autenticar cuando se combinan enlaces [F1-vsf-tr06-2-main A5, A10]. WireGuard comprueba su ventana después de autenticar [F9-wireguard-paper A2].

cengarde reordena por un contador que no puede verificar [razonado]. Por eso:
- un datagrama inyectado con un contador muy adelantado no debe mover la ventana ni vaciar el buffer; se pasa directo, como el caso fuera de rango de MLVPN;
- el reordenador nunca descarta, así que en el peor caso un atacante agrega como mucho la espera máxima.

La regla de cengarde para el reparto (solo datos verificados) se mantiene: el reordenador decide entregas, no reparto.

### 8. De dónde vendrá el desorden en 5G y Starlink
**Celular:**
- Las caídas de señal y los handovers inflan el RTT de un subflujo 3× y hasta 10×. El RTT tarda en volver hasta 7 s caminando, 12 s en auto y 40 s o más con eventos consecutivos (MPTCP sobre dual-LTE) [F8-dual-lte-mptcp A8, A1 corregida].
- minSRTT desvía el tráfico para evitar desorden y desbalancea [A2].
- El umbral de 3 DUPACK se considera suficiente en celular estático [F8-leo-reordering A8].
- Los middleboxes DPI celulares retienen muchos paquetes fuera de orden; es un hallazgo citado de otro trabajo [F8-bufferbloat-3g4g A9].

**Starlink:**
- La reasignación de satélites cada 15 s se infiere de mediciones de latencia [F8-starlink-scheduler-constellations A3]. No hay datos de pérdida ni de desorden en esos bordes.
- En emulación LEO (geometría OneWeb, cambio de ruta cada 30 s), el transitorio tras un cambio de ruta dura de cientos de ms a ~2 s. Puede reordenar cientos de paquetes y no se arregla subiendo el umbral de DUPACK [F8-leo-reordering A1, A2].
- Según los autores, el jitter dentro de una misma ruta (<2,5 ms) no reordena [A4].

Con sondas cada 100 ms, el estimador de OWD ve un pico en 100–200 ms. Lo que ya va en vuelo por ese enlace llega tarde y lo absorbe el reordenador hasta su tope [razonado].

### 9. Qué medir como resultado
Cech insiste en medir la cola fuera de orden junto al goodput, porque dos planificadores con igual goodput difieren mucho en OFO [F3-cech-thesis A3]. TR-348 trata la latencia de reorden como un KPI propio [F2-bbf-tr348 A7].

### 10. SmoothStream
RIST ya reordena con un buffer en tiempo. El presupuesto decidido (cierre + scatter + margen + n_arq × RTT) es lo que pide TR-06-1 §5.3.1 [F1-vsf-tr06-1-simple A6, A8]. El default de 70 ms de la sección de reorden [A11] queda corto frente a los 100–150 ms de spread entre operadores [MEDIR], así que no conviene dejarlo de fábrica. La réplica sin retransmisión solo funciona si el diferencial queda dentro del límite de la clase ST 2022-7 [A12].

Detectar la pérdida a la entrada del buffer da la mínima latencia, pero con desorden dispara retransmisiones de más [A7]. El fec-nack-delay ya decidido tiene que cubrir también el desorden entre patas, no solo dar lugar a la FEC [razonado].

Como idea a medir, no como cambio, dos patrones permitirían ajustar el retardo del NACK por pata:
- el de RACK [F9-rfc8985-rack-tlp A1–A5]: empezar chico, crecer cuando se detecta un NACK espurio (el original llega después de pedirlo), tener tope y volver al inicio tras N recuperaciones;
- la regla de FIFO por pata de la sección 5.

Lo demás de esta familia es coherente con lo decidido:
- SRT fija la latencia en ≈RTT_0/2 + latencia SRT, con TsbpdDelay mínimo de 120 ms, y entrega en orden por marca de tiempo [F1-srt-draft A8, A11];
- RLC descarta lo que llega o se decodifica después de max_lat [F4-rfc8681-rlc A9];
- MPRTP compensa el desfase entre caminos con el retardo de reproducción [F2-mprtp A11].

**Qué medir:**

- Tolerancia real al desorden de los stacks interiores. En banco, inyectar un atraso controlado (5, 20, 50, 100 y 150 ms) a una fracción de los paquetes y medir retransmisiones espurias, bajadas de cwnd y goodput. Probar Linux (RACK), Android, Windows, iOS y macOS, y QUIC de Chrome y de Safari, con Cubic y con BBR.
- Tamaño efectivo de la ventana anti-replay en las implementaciones de WireGuard que se usan de verdad (kernel del OpenWrt de la Pi, kernel del VPS, wireguard-go si lo hubiera), leyendo el código.
- Que el WireGuard emisor saque los contadores en orden aunque cifre en paralelo. Si no, el reordenador vería huecos falsos.
- Comportamiento del reordenador en los rekeys de WireGuard (dos índices vivos a la vez) y con los mensajes sin contador.
- Distribución del tiempo de retención por hueco, huecos soltados por tiempo, paquetes que llegan después de soltar y paquetes descartados por la ventana de WireGuard. Comparar la regla de FIFO por enlace con un timer fijo.
- Supuesto de FIFO por enlace: desorden dentro de un mismo enlace 5G y dentro de Starlink (bordes de 15 s, cambios de ruta).
- Latencia añadida a los paquetes chicos (VoIP, ACK) por bloqueo de cabeza de línea, con y sin bypass, y efecto del bypass en QUIC.
- CPU y memoria del anillo de reorden a la tasa máxima, en la Pi (bajada) y en el VPS (subida).
- Cuántos huecos por segundo genera la pérdida real de cada enlace 5G y de Starlink bajo carga, para estimar cuánto tráfico pagaría la espera máxima sin la regla por enlace.
- SmoothStream: spread de RTT entre operadores y sus colas (p99) para dimensionar la sección de reorden de libRIST, sin dejar los 70 ms de fábrica. Tasa de NACK espurios (el original llega después de pedirlo) como señal para ajustar el fec-nack-delay por pata.

---

## salud

## Eje 1. Estimación y salud de enlaces: capacidad, RTT/OWD, pérdida, cola, radio, silenciado, 5G y Starlink

**En una línea.** Las fuentes coinciden en el qué: cada enlace necesita su propia telemetría, medida por su propio camino, y la conmutación necesita histéresis. Casi ninguna mide el cómo con un túnel que no ve flujos sobre 5G o Starlink. Lo medido sobre los enlaces sale de la familia F8 (Starlink, LTE/5G, bufferbloat celular). Para estimar capacidad sin ver flujos no hay ninguna validación independiente: solo código (srtla), una norma que avisa que su propio estimador sobrestima (SRT), un vendedor (Peplink) y una patente (Dejero).

### 1. Medir el retardo de cada enlace por su propio camino y en cada sentido
- **Sondas propias en cada enlace, también en los que no llevan datos.**
  - Hellos por ambos enlaces en cada intervalo [F2-rfc8157-gre-bonding A7]. El control viaja por el mismo camino que los datos, así que el RTT es el del camino real [F2-rfc8157-gre-bonding A10].
  - RTT Echo de RIST: es opcional, y quien responde puede imponer 100 ms mínimos entre respuestas [F1-vsf-tr06-3-advanced A8] [F1-vsf-tr06-1-simple A10].
  - MPRTP usa informes por subflujo porque el agregado no da RTT ni pérdida por camino [F2-mprtp A3]. Esos informes cuestan ancho de banda de control [F2-mprtp A12].
- **Si la confirmación vuelve por otro camino, el RTT se distorsiona.**
  - QUIC multipath recomienda sumar los dos retardos de ida [F2-quic-multipath A6]. Si cambia el camino del ACK, el mínimo queda desfasado; el draft dice «can», no «debe» [F2-quic-multipath A7].
  - Con retorno asimétrico, MPRTP puede subestimar el RTT hasta la mitad. Es razonamiento del draft, sin medida [F2-mprtp A9].
  - Chuat supone que cada dato se confirma por el mismo camino por el que llegó [F4-deadline-aware-multipath A6].
  - TR-348 pide vigilar cada sentido por separado, porque un camino puede degradarse en uno solo [F2-bbf-tr348, corrección A13].
- **cengarde** ya mide el OWD por sentido con sondas autenticadas, y cada extremo decide su sentido (historia 006). La evidencia lo respalda y no hereda el sesgo de los ACK cruzados.
- **SmoothStream** usa RTT (rtt-drop). En un flujo de subida la cola que importa sí entra en el RTT; el otro sentido solo agrega ruido [razonado].

### 2. Filtrar: promedio, mínimo y máximo
- **Promedio móvil (EWMA).**
  - SRT usa 7/8–1/8, con RTT inicial de 100 ms [F1-srt-draft A6].
  - MLVPN usa α 1/8 y β 1/4, y descarta muestras de 5 s o más [F5-mlvpn A9].
  - cengarde usa ganancia 1/4 (historia 006).
- **El promedio llega tarde.** En simulación, QAware ve que minSRTT y ECF necesitan varias actualizaciones de RTT para notar una cola local llena; la ocupación de la cola local lo muestra casi al instante [F3-qaware A9, matizada: simulación VII.D] [F3-qaware A1, A5].
- **Mínimo y máximo de ventana (BBR).**
  - BBR toma el mínimo del RTT en 10 s, porque el ruido de radio, las colas y la agregación de ACK lo sesgan hacia arriba [F9-bbr-draft A4].
  - Toma el máximo de la tasa entregada sobre dos ciclos, porque la tasa de radio varía mucho [F9-bbr-draft A3].
  - Ese filtro se adapta lento a cambios en escalón, como los cambios de MCS [F4-joint-sched-coding-parallel N1].
  - BBR es un borrador expirado y sin estatus.
- **Trampa medida: un RTT bajo no prueba que el enlace esté sano.**
  - Con pérdida de más de 0,5 % en una ruta (banco gigabit sin retardo añadido), el sRTT de esa ruta cae casi a cero [F3-cech-thesis A7]. El autor lo atribuye a que el control de congestión deja pocos paquetes en vuelo; es inferencia suya [F3-cech-thesis A8].
  - Una sonda de cengarde sobre un enlace con pérdida pero vacío también da un OWD bajo. Por eso la pérdida tiene que ser una entrada aparte [razonado].
- **Propuesta [razonado].**
  - Línea base: mínimo de ventana del OWD de cada enlace.
  - Señal de cola: OWD suavizado menos la base.
  - En Starlink la base cambia cada 15 s (§7). La ventana del mínimo tiene que ser menor que 15 s, o reiniciarse en cada borde. Un mínimo de 10 s como el de BBR leería el escalón como cola.

### 3. Cola y bufferbloat de cada enlace
- **En celular la cola se llena sin que la pérdida avise.**
  - EVDO con 3,1 Mbps de pico y 150 ms de RTT mínimo tiene un BDP de ~58 KB. Aun así, con TCP basado en pérdida se ven latencias de hasta 10 s [F8-bufferbloat-3g4g A12].
  - Ajustar la ventana del receptor bajó el RTT entre 24 y 49 %, con throughput parecido (diferencia máxima de 4 %). Condiciones: redes de BDP chico, 2012 [F8-bufferbloat-3g4g A7, matizada].
  - LTE en movimiento: a alta velocidad, el 75 % de los paquetes MPTCP tiene hasta 1,7× el RTT de retraso de cola. Los autores lo atribuyen a la estación base [F8-dual-lte-mptcp A7].
- **Starlink.**
  - En reposo, el RTT tiene una mediana de 46–52 ms.
  - Bajo HTTP/3 masivo, la mediana sube a 95/104 ms, el p95 a 175/237 ms y el p99 a 210/310 ms (bajada/subida). Es un solo enlace, en Europa, en 2022 [F8-first-look-starlink A11, A2].
  - Mohan afirma que Starlink sufre bufferbloat (Takeaway #1, con datos de M-Lab). Pero sus cifras de OWD de subida (52±14 frente a 27±7 ms) son de llamadas de Zoom, que según el mismo párrafo no deberían sufrirlo [F8-mohan-starlink-multifaceted A8, matizada].
- **Cómo se detecta.** Quien la detecta mide la latencia relativa a la de reposo.
  - Peplink DWB marca congestión por encima de 2× la latencia en reposo: de 30 a 60 ms, con una perilla Low (45 ms) y High (75 ms) y un umbral fijo opcional [F6-peplink-speedfusion-whitepaper A5, N7].
  - Dejero baja la tasa de un enlace cuando su OWD pasa un umbral o cuando los datos no llegan [F7-dejero-us10028163 A4].
  - Raiciu limita la cwnd cuando el sRTT llega a 2× la base; en simulación eso reduce la memoria a la mitad [F3-raiciu-nsdi12 A8].
  - Lo de Peplink es de vendedor y lo de Dejero, de patente; lo de Raiciu es una simulación. Ninguna es una medida independiente en 5G o Starlink.
- **Dónde se controla.**
  - CoDel usa un objetivo de 5 ms, que no puede ser menor que el tiempo de un MTU al ritmo del enlace [F9-rfc8290-fq-codel A1]. Su intervalo es del orden del peor RTT del cuello: 100 ms por defecto [F9-rfc8290-fq-codel A2].
  - En cengarde la cola real está en el módem o en el operador (historia 006, pendiente «cola remota»). El anillo de cada hilo de enlace solo se llena si cengarde es el cuello.
  - Para controlar desde la Pi el bufferbloat de 5G y Starlink en bonding hay que espaciar cada enlace un poco por debajo de su capacidad. Así la cola se forma en el anillo propio, y ahí se aplica un tiempo de permanencia tipo CoDel [razonado].
  - Ese lazo depende de estimar la capacidad (§4), y ninguna fuente independiente lo valida [MEDIR].
  - Las sondas deberían salir por delante del anillo de datos. Así el OWD mide la red, y la permanencia en el anillo mide la cola local [razonado].
- **Redundancia frente a bonding.**
  - En redundancia pura cada enlace lleva todo el caudal. El de menos capacidad se llena primero y el silenciado lo saca: en laboratorio, una cola de 500 ms se silenció en 3–3,6 s (historia 006).
  - En bonding, el silenciado relativo al enlace más rápido no ve un hinchazón común a todos: cuando el bonding los satura, todos van tarde a la vez. Hace falta también la señal relativa a la base de cada enlace (§2) [razonado].

### 4. Capacidad de cada enlace sin ver flujos
- **SRT.** Cada 10 ms el receptor manda un ACK con RTT, tasa de recepción y capacidad estimada [F1-srt-draft A7, matizada]. El propio borrador avisa que esa capacidad puede sobrestimar mucho la real [F1-srt-draft A13].
- **srtla** no estima ni capacidad ni RTT.
  - Cada enlace tiene una ventana de 1 a 60 paquetes, que empieza en 20.
  - La ventana sube con los ACK SRTLA: +0,029 paquetes en el enlace dueño si estaba lleno, y +0,001 en todos por cada secuencia reconocida, o sea +0,010 por ACK de 10.
  - Baja 0,1 por cada NAK [F5-belabox-srtla A2, matizada; A3].
  - Los paquetes en vuelo se recalculan con cada ACK SRT acumulativo [F5-belabox-srtla N2].
  - Un enlace se da por vivo si llegó algo en los últimos 4 s [F5-belabox-srtla A8].
  - srtla-send-rs elige, paquete a paquete, el enlace con mayor ventana/(en vuelo+1) [F5-srtla-send-rs, corrección A12].
- **Dejero (patente).**
  - Al arrancar hace una prueba de ancho de banda, OWD y pérdida, y de ahí saca la tasa ideal [F7-dejero-us10028163 A3].
  - Después vigila varias señales a la vez: latencia, RSSI, fallos de entrega, tasa entregada frente a enviada y backlog [F7-dejero-us10028163 A5].
  - Una patente no prueba que el producto lo haga, y no da pesos ni umbrales.
- **Bedin.**
  - Para estimar capacidades grandes hacen falta ráfagas que saturen el canal, a costa de más retardo. BBR sube el ritmo para detectar capacidad, pero se adapta lento a los escalones. Además, el retardo de la realimentación ensucia la estimación [F4-joint-sched-coding-parallel N1].
  - Un enlace poco usado da estadísticas pobres, se usa todavía menos y puede quedar inutilizable [F4-joint-sched-coding-parallel A8].
- **Peplink (vendedor, sin datos).** Recomienda medir todos los enlaces a la vez y durante 5–10 min, no uno por uno ni 20–30 s, por las fluctuaciones de 5G/LTE y Starlink [F6-peplink-speedfusion-whitepaper N5].
- **BBR en emulación.** En Mininet (2×100 Mbps, tráfico bulk, escenario homogéneo), las estimaciones de BBR dieron el SRTT más bajo y poca cola fuera de orden [F3-mptcp-sched-eval-2025 A9].
- **Propuesta [razonado].**
  - Capacidad de un enlace: máximo de ventana de la tasa entregada, medida por el receptor y devuelta en la sonda.
  - Esa tasa vale como capacidad solo cuando el exceso de OWD sube (enlace saturado). Con el exceso plano es solo una cota inferior.
  - En redundancia pura la tasa entregada es el mínimo entre lo ofrecido y la capacidad, y no hace falta más.
  - En bonding hay que sondear de a un enlace, subiendo su peso un rato, a costa de un pico de cola.
- **Ojo con el código actual.**
  - `cg_probe_info` ya lleva `rx`, `wins` y `lag_us`. Pero `rx` se llena con wins + dups, y las copias posteriores se cuentan antes del MAC.
  - `arrival.h` dice que esos números son solo para mostrar, y que un duplicado sin verificar puede quitarle el «win» a otro enlace [código: engine/src/arrival.h, client.c:1010, server.c:456].
  - Un estimador de capacidad para bonding necesita un contador solo de paquetes verificados (regla del diseño A.7).
- La señal de radio no sirve para estimar la capacidad (§8).

### 5. Pérdida de cada enlace
- **MLVPN.**
  - Mide la pérdida sobre las últimas 64 secuencias propias de cada enlace. Con el valor por defecto solo declara LOSSY con 64 de 64 perdidas [F5-mlvpn A7, matizada].
  - Un salto de más de 64 en la secuencia se trata como reinicio y deja la pérdida en 0 % [F5-mlvpn N1]. La ráfaga larga, que es el evento que importa, queda invisible.
  - Starlink sí tiene caídas de más de 1 s y ráfagas de más de 100 paquetes, en la prueba de mensajes y también en H3 [F8-first-look-starlink, corrección A6].
- **Medida ausente.** En el espejo Rel-19 de ATSSS, una medida ausente cuenta como no excedida; la versión V17.9.0 no trae esa regla [F2-3gpp-ts24193-atsss A6, matizada].
  - cengarde hace lo contrario: sin informes frescos no decide nada (historia 006).
  - srtla-send-rs también: sin RTT, la ventana cae a su techo [F5-srtla-send-rs A2].
- **Chuat** estima la pérdida de cada camino como perdidos sobre enviados, empezando en 0 % [F4-deadline-aware-multipath A7].
- **Pérdida no es congestión.**
  - Desde el transporte no hay forma conocida de distinguir pérdida por congestión de pérdida del medio [F8-first-look-starlink A7].
  - En Starlink con carga, las pérdidas son frecuentes y cortas: mediana de 49 µs y p99 de 7,5 ms; los autores conjeturan congestión. Sin carga son raras y largas [F8-first-look-starlink A4, A5].
  - Peplink trae la opción «Ignore Packet Loss Event» para enlaces con pérdida conocida; o sea, por defecto trata la pérdida como congestión [F6-peplink-speedfusion-whitepaper A6].
  - Con pérdida aleatoria, un control de congestión basado en pérdida rinde muy poco: con 1 % de pérdida y 100 ms de RTT, CUBIC no pasa de ~3 Mbps. Es el modelo de RFC 8312, citado por el draft de BBR [F9-bbr-draft A7].
- **cengarde [razonado].**
  - En redundancia, cada contador sale por todos los enlaces vivos. `arrival.h` ya cuenta, en cada enlace, los contadores que nunca llegaron (`missed`, solo para mostrar).
  - En bonding, cada contador va por un solo enlace, y el receptor no sabe a cuál le faltó. El emisor tendría que anunciar en la sonda cuántos datos mandó por ese enlace en el intervalo, para compararlo con lo recibido verificado, como el informe de emisor de RTCP.
  - Eso cabe en las sondas sin tocar la cabecera de datos.

### 6. Silenciado, conmutación y vuelta
- **Las fuentes coinciden en la forma.**
  - Conmutar solo por un deterioro real y sostenido, con una alternativa mejor, y volver con histéresis [F2-bbf-tr348 A9]. Es guía, no MUST: lo obligatorio es solo vigilar la conectividad [F2-bbf-tr348 A8, matizada], y la tabla de KPI no dice cómo estimarlos [F2-bbf-tr348 A10].
  - Dejar de repartir si la diferencia de retardo entre enlaces pasa un umbral durante un tiempo [F2-rfc8157-gre-bonding A9].
  - Mantener vivos los caminos que no llevan datos [F2-mprtp A4, A5] [F1-srt-socket-groups A5].
  - Si el camino primario cae, mandar por otro [F2-mp-dccp A4].
  - El silenciado de cengarde tiene exactamente esa forma: 150 ms detrás del más rápido durante 2 s para silenciar, 4 s por debajo de 120 ms para volver, y una espera que se duplica en cada recaída.
- **Tiempos en SRT.**
  - Un enlace es inestable si pasa más de un timeout entre dos respuestas del receptor [F1-srt-socket-groups A3], y se sigue usando mientras tanto [A4].
  - Ese timeout vale min(max(60 ms, 2·SRTT+4·RTTVar), latencia). El enlace se da por roto tras 5 s inestable [F1-srt-socket-groups A12, A8].
  - La latencia debe ser al menos 2× ese timeout [A6].
  - Los «50 ms» de pruebas en red local no traen datos [A7, matizada].
  - El peso decide cuál de los enlaces estables queda activo [A10].
- **Tiempos en srtla-send-rs.**
  - Un enlace queda obsoleto tras 4× su sRTT, acotado entre 1 y 3 s [A2].
  - El piso de 1 s queda por encima de las pausas de 400–800 ms que el autor llama «rutina» en celular bonded, sin medición [A11].
  - Con silencio total y backlog lleno, lo saca en 250 ms [A7].
  - Para volver exige una racha ininterrumpida de prueba de entrega durante 2× la ventana [A3].
- **Otros tiempos.** RFC 8157 declara el túnel caído tras 3 a 10 Hellos sin acuse [A8]. MLVPN usa 60 s por defecto [F5-mlvpn A8]. srtla usa 4 s [F5-belabox-srtla A8].
- **Modo redundant de 3GPP (solo espejo Rel-19).** Si los dos accesos pasan el umbral de RTT, el UE puede duplicar; si lo pasa uno solo, el tráfico va por el otro [F2-3gpp-ts24193-atsss A5, A3, matizadas].
- **Prueba de vida no es prueba de entrega.**
  - srtla-send-rs estampa la prueba de entrega solo con un ACK de una secuencia que ese enlace llevó, o con un keepalive completo; nunca con bytes genéricos [F5-srtla-send-rs A1].
  - srtla, en cambio, da por vivo un enlace con cualquier paquete, incluido el eco del keepalive [F5-belabox-srtla N3].
  - Las sondas de cengarde (~68 B) prueban el camino para paquetes chicos. Un camino celular o con CGNAT que filtra el ICMP de fragmentación puede pasarlas y tirar los datagramas grandes.
  - OMR lo midió en banco: dos caminos de 100 Mbit/s, uno con el ICMP filtrado; 85–90 Mbit/s sin PLPMTUD frente a 152–155 Mbit/s con él. Es TCP [F5-openmptcprouter A5].
  - El detector de mudo de cengarde, que mira sondas, no lo vería [razonado].
- **Fallos de código a no copiar.**
  - MLVPN entra en fallback solo si todos los enlaces están LOSSY o lentos; un enlace caído no cuenta [F5-mlvpn A10, matizada].
  - La documentación no coincide con el código. MLVPN: la man page dice keepalive cada «timeout/2» y el código, cada 1 s [F5-mlvpn A8]. srtla-send-rs: el README dice que basta el siguiente keepalive, y el código pide la racha de 2× [F5-srtla-send-rs A3]. Manda el código.
- **Diagnóstico difícil.** MPTCP deja para estudio el reset de subflujos lentos, porque un camino muy asimétrico se diagnostica mal [F2-rfc8684-mptcp A10]. En MP-DCCP, qué tipos de RTT existen depende de la implementación [F2-mp-dccp A8].
- **La vuelta.**
  - Un enlace sin uso tiene estimaciones viejas [F4-joint-sched-coding-parallel A8]. srtla vuelve la ventana a 1 al reconectar (mecanismo de la ficha).
  - En bonding, un enlace que vuelve debería entrar con poco peso [razonado].
  - En redundancia, la vuelta de golpe es correcta (historia 006: la rampa queda para el WRR).

### 7. Starlink
- **Patrón de 15 s.**
  - La latencia cambia en escalones en los segundos 12, 27, 42 y 57 de cada minuto, en 4 terminales y en todos los periodos medidos. Las ventanas consecutivas son estadísticamente distintas (Mann-Whitney, p<0,05) [F8-starlink-scheduler-constellations A1].
  - Los escalones aparecen también con el terminal muy por debajo de su capacidad. Los autores lo usan como una de tres razones que «sugieren» una reasignación global; no descarta la carga de la celda [A2, matizada].
  - 15 s es poco tiempo para que el movimiento orbital cambie el rendimiento [A4, razonado].
  - Mohan: el OWD salta en los bordes de 15 s [F8-mohan-starlink-multifaceted A2].
  - La reconfiguración es global y sincronizada entre dos terminales con PoP distintos, y «probablemente» independiente del traspaso de satélite [A1, matizada].
  - Con un solo satélite visible, el RTT igual cambia entre intervalos [A4]. El planificador global es una hipótesis [A9].
  - Dentro de cada ventana, la latencia forma bandas separadas por pocos ms (mecanismo de la ficha de Tanveer).
  - Ninguna fuente da la magnitud del salto, ni la pérdida o el reorden en el borde.
- **Cortes.**
  - Michel ve caídas de más de 1 s [F8-first-look-starlink, corrección A6]. Mohan dice que no tuvo cortes de segundos (línea 399).
  - Peplink (vendedor): el terminal se reconecta a un satélite nuevo «p. ej. cada 15–20 s». Recomienda ignorar la pérdida y añadir FEC [F6-peplink-speedfusion-whitepaper, corrección A10].
- **Para cengarde [razonado].**
  - Un escalón en un borde es calendario, no cola. Hay que reiniciar la base del OWD en el borde.
  - Si un escalón deja a Starlink más de 150 ms detrás del más rápido durante más de 2 s, el silenciado lo saca. La vuelta (4 s) cabe en 15 s, pero con la espera que se duplica hasta 32 s puede quedar fuera varios intervalos. En redundancia es aceptable, porque al menos 2 enlaces llevan todo; en bonding es caro.
  - Conviene detectar el borde por el cambio de la base y no por la hora, porque el reloj del router puede estar mal (historia 002).
  - La magnitud en Chile está por medir [MEDIR].

### 8. 5G/LTE y señales de radio
- **Picos de RTT.** Medidos con dos LTE de operadores distintos, una Pi 2 con módems USB CAT4 y en movimiento.
  - 3× tras una caída de señal y hasta 10× tras un handover [F8-dual-lte-mptcp A1, matizada].
  - Un evento de ejemplo: una caída de 8 dBm dio 2× RTT y −22 % de throughput total [A3, matizada].
  - Tiempo de recuperación t_R: hasta 7 s caminando y 12 s en auto, y 40 s o más con handovers y caídas seguidas en los dos enlaces (texto, líneas 285 y 300–304).
  - Tendencia «determinista»: un pico de 1,7 por cada 2 dB de caída. Se mide en el mismo evento; no es una señal adelantada [A5, matizada].
  - Entre dos LTE no hay un camino que sea siempre el mejor [A9].
  - Los autores proponen un planificador que vigile los eventos de última milla, sin probarlo [A12].
  - Son datos de movilidad; el CPE de cengarde es fijo. Eso significa menos traspasos, pero la carga de la celda sigue variando [razonado].
- **RRC.**
  - Promoción desde IDLE: 341 ms (SA), 1440 ms (NSA) y 1907 ms (Verizon NSA mmWave).
  - Timer de inactividad: 5 s (T-Mobile 4G) y 10,2–10,5 s en la mayoría [F8-variegated-5g A4]. Son teléfonos en EE. UU., 2021.
  - Con sondas cada 1 s, el enlace no debería caer a IDLE [razonado].
  - Si alguien sube `probe_idle_ms` por la cuota a más de ~5 s, el primer paquete tras el reposo podría pagar la promoción y parecer un salto de OWD o un mudo [razonado, MEDIR en el CPE].
- **La señal no predice el throughput.**
  - En traspasos medidos conduciendo no hay correlación aparente entre ∆RSRP y ∆throughput. Se juzga sobre el gráfico, sin coeficiente [F8-handover-5g-2025 A1, matizada].
  - En los traspasos intra-RAT la RSRP suele mejorar, pero ∆T es negativo en el 40–50 %. No hay línea base [A2, matizada].
  - Verizon 5G→LTE: la RSRP mejora en el 85 % de los casos, pero el throughput cae en el 80 %, y en el 18 % cae más de 150 Mbps [A3].
  - LTE→5G: la señal baja en el 45–75 % de los casos, pero ∆T es negativo solo en el 20–30 % [A4].
  - Los autores concluyen que es muy difícil predecir el throughput por la señal [A5].
  - Peplink ve usuarios con 100 % de señal y poco ancho de banda [F6-peplink-speedfusion-whitepaper N4] (vendedor).
  - Dejero cuenta el RSSI como una señal entre varias [F7-dejero-us10028163 A5] (patente).
  - Speedify usa la intensidad Wi-Fi en iOS para predecir cambios de red [F6-speedify-streaming-mode S1]. Es vendedor, no es celular y no trae datos.
- **Lectura [razonado].** El nivel de señal no sirve para estimar la capacidad. Lo que podría servir son los eventos discretos: cambio de celda, de banda o de RAT, sobre todo 5G→LTE, cuyos efectos están medidos y son bruscos. Ninguna fuente prueba que la señal se adelante al RTT o a la pérdida.

### 9. Pérdida de RF frente a congestión (para el gobernador T7)
- **Nadie lo resuelve con datos.**
  - Desde el transporte no se pueden distinguir [F8-first-look-starlink A7].
  - Peplink lo deja en una perilla manual [F6-peplink-speedfusion-whitepaper A6].
  - En celular la congestión aparece como retardo sin pérdida [F8-bufferbloat-3g4g A12].
  - Tetrys pide que la tasa de FEC se adapte junto con el control de congestión, para que la reparación no congestione [F4-rfc9407-tetrys, corrección A8].
- **Regla candidata [razonado].**
  - Pérdida con el exceso de OWD plano: es del medio, y más FEC ayuda.
  - Exceso de OWD que sube, con o sin pérdida: es cola, y hay que bajar el peso o la tasa.
  - Un evento de radio en la misma ventana refuerza la lectura de medio.
  - Hay que medirla en banco (netem: pérdida aleatoria frente a límite de tasa con cola) y en campo [MEDIR].

**Qué medir:**

- En cada enlace (WOM, Entel, Claro, Movistar y Starlink) y en cada sentido: OWD base en reposo, OWD con el enlace saturado (cuánto se hincha), y su p95 y p99. Con las sondas de cengarde y con carga simultánea en todos los enlaces durante 5–10 min, como sugiere Peplink N5.
- Starlink en Chile. Medir la magnitud del escalón de OWD en los bordes de 15 s, en cada sentido. Medir la pérdida y el reorden en ±500 ms alrededor del borde, y la frecuencia de cortes de más de 1 s. Ver si eso dispara el silenciado (150 ms durante 2 s) y cuántos intervalos queda fuera con la espera de hasta 32 s.
- Agujero negro de MTU. En cada operador y en Starlink (IPv4 con CGNAT e IPv6), ver si pasan datagramas de 1420–1500 B mientras pasan las sondas de 68 B. Probarlo con sondas rellenadas.
- RRC en los CPE. Medir, en cada operador, el OWD del primer paquete tras 1, 5, 10 y 20 s de reposo, para fijar un valor seguro de `probe_idle_ms`.
- En los CPE fijos: con qué frecuencia cambian de celda, de banda o de RAT, y qué pico de OWD y qué tiempo de recuperación trae cada cambio, por operador.
- Estimador de capacidad (máximo de la tasa entregada verificada más el exceso de OWD) frente a iperf en cada enlace. Medir su error, su tiempo de reacción a un escalón de capacidad y su costo de CPU en la Pi.
- Lazo de pacing más permanencia tipo CoDel en el anillo de cada enlace. Medir latencia y throughput frente a no hacer pacing, con un enlace 5G y uno Starlink saturados, y la CPU en la Pi con hilos por enlace.
- Discriminador de RF y congestión, primero con netem: pérdida aleatoria frente a un límite de tasa con cola. Ver si separa bien la regla «pérdida con OWD plano» frente a «OWD que sube». Después, en campo.
- SmoothStream: adelanto de la radio Quectel. Correlación cruzada entre SINR, RSRQ y eventos de celda, y el RTT, la pérdida y los NACK de cada pata, con resolución de 100 ms. Medir también el costo de leer por AT en el ARM.
- SmoothStream: tiempo de reacción de rtt-drop (RTT suavizado de libRIST) ante un escalón de cola de 500 ms, en un escenario equivalente al «health» de cengarde.
- Pausas de entrega «rutinarias» en celular: srtla-send-rs dice 400–800 ms, sin medición. Medir la distribución de huecos de entrega en cada operador.

---

## correlacion

## Eje 2. Correlación de pérdidas y diversidad entre enlaces

**En una línea.** Es el eje con menos evidencia. Hay una sola medición de campo de correlación entre operadores: Salzburgo, con dos operadores, en un coche, por autopista. Todos los modelos de planificación con FEC o ARQ suponen independencia. Lo que sí queda claro es que suponer independencia sobrestima la ganancia de duplicar, y que el piso de pérdida lo fijan los eventos comunes, no el número de enlaces.

### 1. La única medida: dos operadores en un coche
- **Pérdida.**
  - Una muestra cuenta como pérdida si el paquete no llega o si su RTT pasa de 100 ms [F8-salzburg-multiprovider A8].
  - Pérdida del operador A: 0,00856. Del operador B: 0,02562.
  - Si fueran independientes, la pérdida en los dos a la vez sería 0,00022. La medida fue 0,00077 (IC 95 %: 0,00062–0,00094), o sea 3,5× [F8-salzburg-multiprovider A3].
- **Caída de tasa.**
  - Definida como no poder transmitir 1 Mbit en 1 s; el umbral es arbitrario de los autores.
  - A: 0,01086. B: 0,01599. Si fueran independientes: 0,00017. Medida: 0,00071, o sea 4,2× [F8-salzburg-multiprovider A4].
- **Alcance.** Dos operadores de Austria, en un coche por autopista, datos de 2021–2022, con latencia y tasa medidas en días alternos [F8-salzburg-multiprovider A7]. No hay cuatro enlaces ni hardware compartido.
- **La correlación cambia.** En la práctica es desconocida y no constante [F8-salzburg-multiprovider A1].
- **Modelo.** r = r1 + r2 − r1·r2 − c·√(r1·r2·(1−r1)·(1−r2)) [F8-salzburg-multiprovider, corrección A2]. En el ejemplo ilustrativo, con canales de 99 %, c = 0,1 baja la fiabilidad un orden de magnitud [A5].
- **c implícito.** Sale ≈ 0,038 para la pérdida y ≈ 0,041 para la caída. Es un cálculo de la ficha, no del paper; lo rehice con la ecuación (2) y cuadra [razonado].

### 2. Por qué un c chico pesa tanto [razonado]
Con pérdidas chicas, P(las dos) ≈ p1·p2 + c·√(p1·p2). El término correlacionado es c/√(p1·p2) veces el término independiente. Con p1 = p2 = 1 % y c = 0,04, la pérdida conjunta es ~5× la independiente. Con las cifras de Salzburgo, la fórmula da exactamente los 3,5× medidos. Consecuencias:
- duplicar baja la pérdida de p a ≈ p·(p + c), no a p². La segunda copia gana mucho, pero hay un piso correlacionado;
- cuanto mejores son los enlaces por separado, más pesa la correlación;
- con 4 o 5 enlaces, el c entre pares no alcanza. P(todos tarde a la vez) lo fijan los eventos de modo común, y un c entre pares no los describe.

### 3. Lo que suponen los modelos
- **Todos suponen independencia.**
  - Bedin supone enlaces independientes; la correlación (p. ej. bloqueos geométricos en mmWave) queda como extensión futura. Que los dos mmWave coincidan bloqueados con pout ≥ 0,2 es coincidencia de canales independientes, no correlación [F4-joint-sched-coding-parallel A6].
  - Garrido supone pérdidas i.i.d. e independientes entre caminos [F4-joint-sched-coding-delayed-feedback A7].
  - Chuat supone retardos independientes [F4-deadline-aware-multipath A11].
  - QUIC-FEC modela cada camino con Gilbert-Elliott por separado y supone rara la pérdida simultánea; ese supuesto sostiene su cálculo [F4-quic-fec-mpquic, mecanismo de la ficha].
- **Sus ganancias son cotas superiores** mientras no se mida la correlación [razonado]. Por ejemplo, la paridad entre enlaces que compensa la caída de uno, o el reparto round-robin con RS, que en el ejemplo analítico sube la recuperación de una ráfaga de 3 de 1/3 a 2/3 [F4-quic-fec-mpquic, corrección A6].
- **La diversidad entre caminos no siempre paga.** Con RLC, la ventaja del round-robin se ve menos [F4-quic-fec-mpquic N1]. Con caminos de pérdida distinta y retardo parecido, el round-robin no supera claramente a un solo camino [F4-quic-fec-mpquic, corrección A7].
- **RFC 8681.** Un bloque más grande aguanta mejor las ráfagas a costa de más latencia [F4-rfc8681-rlc A2]. La ventana de la FEC compite con la duración de un evento común.

### 4. Cuellos y fallos compartidos
- **Caminos que no son disjuntos.**
  - No hay garantía de que los caminos sean disjuntos [F2-quic-multipath A9]. Con un cuello compartido y control de congestión estándar, la conexión se puede quedar con más de lo justo [F2-mp-dccp A11, matizada]. Por inferencia, la suma de las capacidades sobrestima el total [razonado].
- **cengarde tiene tres puntos de modo común:** las 4 VLAN en un solo adaptador USB gigabit (historia 004), una sola Pi y un solo VPS.
  - Un reset del USB, o un problema en el VPS, se lleva todos los enlaces a la vez, y ninguna diversidad de operador lo cubre [razonado].
  - Ninguna fuente mide enlaces que comparten hardware [F8-salzburg-multiprovider A7].
- **LTE.**
  - Los dos LTE sufren cambios de última milla en momentos y con frecuencias distintas, que a menudo se solapan (texto, líneas 42–44) [F8-dual-lte-mptcp].
  - Los handovers afectan a los dos caminos de MPTCP, con −74 % de throughput (líneas 310–311). Una caída de señal, en cambio, solo afecta a su subflujo [F8-dual-lte-mptcp A3]. Lo de los handovers probablemente es acoplamiento de MPTCP y no correlación de radio [razonado].
  - Con eventos seguidos en los dos enlaces, la recuperación tarda 40 s o más (línea 285).
- **Starlink.**
  - La reconfiguración es global y sincronizada: dos terminales con PoP distintos comparten los bordes [F8-mohan-starlink-multifaceted A1, A9] [F8-starlink-scheduler-constellations A1].
  - Por eso dos Starlink no se diversifican entre sí frente a esos eventos [razonado].
  - Starlink frente a 5G son sistemas distintos y se espera baja correlación, pero nadie lo midió.
- **Peplink (vendedor).** Recomienda usar al menos dos ISP por diversidad de proveedor, pero no habla de pérdidas [F6-peplink-speedfusion-whitepaper A11]. Menciona que varias SIM en la misma torre se reparten su cuota, con una aritmética inconsistente [N4].

### 5. Qué significa
**cengarde**
- El ROADMAP pide una FEC que sobreviva la caída de 1 de 4 enlaces con ≤1,34×, y también k-de-N. Ese objetivo modela que «cae un enlace»; un evento del USB o del VPS es que «caen varios». No hay evidencia de cuál pesa más.
- **Instrumento casi gratis [razonado].** Hoy cada contador sale por todos los enlaces. `arrival.h` ya guarda para cada contador la máscara de enlaces esperados, la de los que entregaron y la hora de la primera copia [código].
  - Un histograma de máscaras de faltantes (32 contadores para 5 enlaces) y un plazo en tiempo dan P(todos tarde) para cualquier subconjunto. O sea, el número que decide k-de-N y cuántas copias llevan los ACK, la VoIP y los handshakes.
  - Solo sirve para medir: las copias posteriores no están verificadas, así que no puede guiar el reparto.
- **k-de-N:** elegir los k enlaces por la pérdida conjunta medida, no solo por el OWD de cada uno [razonado].

**SmoothStream**
- El minimax con 1,60× cubre la caída de la peor pata. Si en el estadio las patas caen juntas, la idea de usar menos patas con más FEC, o de duplicar en las dos mejores, depende de una correlación que nadie midió en ese escenario.
- Las dos mejores por separado pueden ser las más correlacionadas, por ejemplo si comparten torre o banda [razonado].
- El c ≈ 0,04 de la autopista no se puede trasladar al estadio [MEDIR].

**Qué medir:**

- cengarde: matriz de pérdida y tardanza conjunta para cada subconjunto de los 5 enlaces (histograma de máscaras de faltantes en arrival.h), con plazos de 50, 100 y 150 ms desde la primera copia. Por hora del día y durante varias semanas, con la razón frente a la independencia y el c entre pares; comparar con el 3,5× de Salzburgo.
- cengarde: eventos de modo común. Medir la frecuencia y la duración de los huecos simultáneos en las 4 VLAN del USB (adaptador, driver o Pi) y en los 5 enlaces (VPS).
- cengarde: cuello compartido. Cargar un enlace con iperf y medir el OWD y la pérdida de los otros; medir también el total máximo del adaptador USB con 4 VLAN en la Pi.
- Starlink frente a 5G: ver si los eventos de 5G coinciden con los bordes de 15 s de Starlink (lo esperado es que no).
- SmoothStream, en el estadio con la celda saturada y en una calle normal como control: pérdida conjunta de las 4 patas con un plazo igual al buffer de RIST, y P(2 o más, 3 o más y las 4 patas tarde a la vez). Registrar celda, banda y PCI de cada Quectel para ver si comparten sitio.
- SmoothStream: ver si los eventos de radio (caídas de SINR, cambios de celda) ocurren a la vez entre operadores y si anticipan las pérdidas conjuntas.
- Estabilidad en el tiempo: Salzburgo dice que la correlación no es constante, así que hay que repetir la medida en días, horas y eventos distintos.
- Para decidir k-de-N o con qué duplicar: comparar P(las dos tarde) del mejor par por pérdida conjunta con la del mejor par por OWD individual.

---

## FEC en multicamino (dónde y cómo repartir la paridad, ventana frente a bloque, ratio adaptativo, distinguir RF de congestión)

## FEC en multicamino

**Estado de la evidencia.** Nadie mide FEC multicamino en una red real. Lo que hay son normas sin medidas (RFC 8681, RFC 9407), una emulación (QUIC-FEC), simulaciones (Chuat en ns-3, Garrido), un modelo analítico (Bedin) y código sin banco de pérdidas (kcp-go).

- **Spread de RTT.** Ninguna fuente prueba caminos con 100–150 ms de diferencia de RTT. QUIC-FEC usa «similar delays for both paths» (§7) [F4-quic-fec-mpquic], y Garrido supone el mismo retardo por camino [F4-joint-sched-coding-delayed-feedback].
- **Reparto de la paridad.** Ninguna norma ni código revisado dice cómo repartirla entre caminos:
  - RFC 8681 modela un solo punto de decodificación [F4-rfc8681-rlc A12];
  - RIST Advanced solo dice que ST 2022-1/-5 «can be added» [F1-vsf-tr06-3-advanced A9];
  - kcp-go protege una sola sesión [F5-kcp-go A6];
  - srtla, MLVPN y OMR no tienen FEC [resumen F5].

El minimax de SmoothStream sigue siendo trabajo propio.

### 1. Ventana o bloque
- **La ventana deslizante repara una pérdida aislada con el siguiente símbolo de reparación,** sin esperar al fin del bloque. RFC 8681 lo dice en texto informativo que remite a Roca17, no lo mide [F4-rfc8681-rlc A1]. Tetrys dice lo mismo, sin datos [F4-rfc9407-tetrys A2].
- **La única comparación medida** es una emulación de un camino, con Gilbert-Elliott y búfer de 33–100 ms [F4-quic-fec-mpquic]:
  - RLC (3,2,20) entrega más datos con ráfagas cortas, y RS (30,20) gana con ráfagas largas (A1);
  - en rebuffering, sobre 120 configuraciones, RS gana la mayoría por las ráfagas medias largas (A2);
  - con pérdida uniforme de 0–3 % y búfer de 33 ms gana RLC (A3, A4).

  No se contradicen: miden cosas distintas en rangos distintos.
- **Un bloque necesita reloj.** kcp-go guarda como mucho 3 bloques y descarta los viejos por número de secuencia, sin mirar el tiempo (A5). Tampoco genera paridad si pasan más de 500 ms entre datos (A2). Es la falla del bloque que no cierra en un flujo a ráfagas.
- **Dimensionado desde el plazo** (Apéndice C, informativo) [F4-rfc8681-rlc]:
  - para CBR con tasa de salida conocida, dw_max = max_lat·br_out·cr/(8·E), y con WSR = 191 la ventana de codificación queda en ~0,75 de la de decodificación (A3, matizada);
  - los símbolos más viejos que max_lat·WSR/255 deberían (SHOULD) salir de la ventana (A4);
  - el receptor guarda 2·dw_max, con un piso de 40 símbolos (A11).
- **Cuerpo y densidad.**
  - GF(2^8) protege más. GF(2), que es XOR, es más barato, pero la protección «is significantly reduced» (A7).
  - GF(2) con DT = 15 es el XOR de toda la ventana: un solo símbolo de reparación útil por ventana (A8, matizada).
  - La guía de DT es cualitativa (A6), así que la fórmula de DT de SmoothStream es propia.
- **CPU.**
  - El único dato en ARM es de Roca17, citado por el RFC: decodificación de 745 Mbit/s a 2,8 Gbit/s en un Cortex-A15, con ventana de 18 o 23 símbolos [F4-rfc8681-rlc].
  - kcp-go, RS 10:3 con paquetes de 1500 B en un Ryzen 9 5950X, en microbenchmark sin red: 181 ns por paquete al codificar y 679 ns al decodificar con ~7,7 % de pérdida simulada [F5-kcp-go].
  - En una Pi 4 (Cortex-A72) no hay nada medido.

### 2. Dónde va la paridad
**A favor de repartirla:**
- Con enlaces independientes, la paridad que viaja por uno compensa la caída de otro, si alcanza la redundancia. Es razonamiento del paper [F4-joint-sched-coding-parallel A1].
- Con RS y dos caminos en round-robin, una ráfaga de 3 símbolos en un camino se recupera en 2/3 de las posiciones de inicio, frente a 1/3 con un solo camino. Vale si las pérdidas simultáneas son raras, y es un ejemplo analítico [F4-quic-fec-mpquic A6, corregida].
- FMTCP reparte símbolos fountain entre subflujos. Es idea de diseño, y sus números no se leyeron [F4-fmtcp A2].

**Matices y en contra:**
- **Con RLC, repartir rinde menos.** La ventaja «is less visible»: intercalar caminos durante una ráfaga la alarga virtualmente entre ecuaciones interdependientes, aunque lo compensa en parte que lleguen símbolos fuente durante la ráfaga. Por eso el paper sigue solo con RS (§7.1, Fig. 14; lo comprobé en el texto) [F4-quic-fec-mpquic].
- **Ráfagas largas y caminos distintos.**
  - Con RS y ráfagas medias largas, el multicamino tuvo más rebuffering que un solo camino (Fig. 13).
  - Con caminos de pérdida distinta, round-robin no le gana con claridad a un camino. Los autores piden usar el mejor camino cuando difieren y alternar cuando se parecen (A7, corregida).
  - Su HighRB, un sorteo ponderado por bytes libres de cwnd, gana casi siempre, salvo cuando un camino es mucho peor (§7.3, comprobado).
- **Recuperar un grupo repartido obliga a esperar al camino más lento.** Chuat lo dice así: «the delay required to recover the corresponding group of packets equals the longest delay of all paths». Añade que las pérdidas correlacionadas bajan la eficacia de la FEC, y ve «questionable» la codificación extremo a extremo en multicamino. Es opinión de los autores, sin medida (§IX.B, comprobado) [F4-deadline-aware-multipath].
- **Respaldos débiles y caídas de capacidad.** Un respaldo débil sirve de poco si no puede llevar el bloque entero dentro del plazo (mmWave con respaldo sub-6, en un modelo; comprobado en el texto) [F4-joint-sched-coding-parallel]. Y la FEC no sirve ante una caída de capacidad: con la redundancia, el bloque tarda más (A2).
- **Correlación: hay un solo dato entre operadores** [F8-salzburg-multiprovider, Tablas II y III, comprobado].
  - Condiciones: dos operadores, un coche en autopista en Austria, y datos de conveniencia que los autores llaman preliminares. La pérdida es un paquete perdido o con RTT mayor que 100 ms.
  - La combinación pierde 0,077 %, frente al 0,022 % que daría la independencia (3,5×).
  - La caída, menos de 1 Mbit en 1 s, es 4,2× la que daría la independencia.
  - Con la ec. 2 del paper sale c ≈ 0,04 [razonado].
  - Bedin, Garrido y QUIC-FEC suponen independencia, y el modelo de Chuat también.

**Una cuenta que vale para los dos productos** [razonado]:
- Para sobrevivir la pérdida total de un enlace, la paridad tiene que cubrir al menos los símbolos que llevaba ese enlace.
- Si el enlace más cargado lleva una fracción s del tráfico protegido, la expansión mínima es 1/(1−s): 1,33× con 4 enlaces iguales (s = 0,25) y 1,67× si el mayor lleva el 40 %.
- El objetivo del ROADMAP de cengarde (≤1,34× con 4 enlaces) solo se cumple si ningún enlace lleva más del ~25 % del tráfico protegido.
- El 1,60× medido en SmoothStream con tope de carga corresponde a s ≈ 0,375.

### 3. Ratio adaptativo y RF frente a congestión
- **Tetrys** (RFC Experimental, sin medidas) [F4-rfc9407-tetrys]:
  - la tasa de código PUEDE adaptarse cuando varían el retardo y la pérdida, y tiene que arrancar con redundancia (A7, corregida);
  - dentro del transporte va junto al control de congestión: «an increase in the repair ratio should be done conjointly with a decrease in the source sending rate» (§6.2, citando a RMCAT; comprobado);
  - fuera del transporte, RED-FEC pone más paridad cuando la cola del punto de acceso está más vacía (A8, corregida);
  - debajo del transporte, la FEC puede esconderle al control de congestión de arriba las pérdidas por congestión (§6.1, comprobado);
  - en un túnel que agrega flujos, una redundancia que no distingue flujos puede ir contra lo que necesita cada uno, y el bloqueo de cabeza de línea los golpea a todos (§6.3, comprobado).
- **Garrido** (simulación, un camino, pérdida i.i.d.) [F4-joint-sched-coding-delayed-feedback]:
  - la redundancia predictiva cuesta en general menos del 5 % de capacidad (A2);
  - no se dispara si d < 1/p, con d medido en paquetes en vuelo y no en ms (A3, matizada).
- **Bedin** [F4-joint-sched-coding-parallel]:
  - la FEC no sirve ante una caída de capacidad (A2);
  - el óptimo del modelo da 10–20 % de redundancia, y ninguna con las colas llenas. Es un caso del modelo, no una medida (A7, matizada).
- **Vendedores** (no sostienen nada por sí solos):
  - Peplink ofrece FEC estática de 13,3 % o 26,7 %, o adaptativa de 6,7–20 %, sin geometría del código ni condiciones [F6-peplink-speedfusion-whitepaper A3]. Recomienda usarla junto a la duplicación, salvo en video en movimiento (A4).
  - Su DWB trata la pérdida como congestión por defecto. Con Starlink recomienda activar «Ignore Packet Loss Event» y sumar FEC (líneas 743–744 y 804–809, comprobado). Es una perilla manual, no una señal.
  - LiveU nombra una «Dynamic FEC» sin explicar el mecanismo [F7-liveu-lrt A1].
- **Señales disponibles:**
  - en celular, el bufferbloat hace que la pérdida no avise de la congestión [F8-bufferbloat-3g4g, resumen F8];
  - la pérdida y el retardo crecen con el uso del camino [F4-deadline-aware-multipath §IX.A];
  - Zoom sube su FEC propia de 2±2 a 146±99 kbit/s sobre Starlink, con FPS igual, en llamadas de muestra [F8-mohan-starlink-multifaceted A10, matizada];
  - ningún paper prueba que SINR o RSRP adelanten la pérdida, y la mejora de RSRP en un handover no predice la del throughput [resumen F8, F8-handover-5g-2025].
- **Regla de clasificación propuesta** [razonado; MEDIR]:
  - pérdida precedida de una subida de OWD/RTT en esa pata = cola propia (congestión): bajar el peso o la tasa, no subir la FEC;
  - pérdida sin subida de OWD, junto a un evento de radio (caída de SINR, handover, borde de 15 s de Starlink) = borrado de RF: subir la FEC;
  - pérdida simultánea en varias patas = causa común (celdas saturadas en el estadio o un cuello compartido): la FEC repartida rinde poco, y conviene bajar la tasa del encoder o concentrar en menos patas.

### 4. Integración
Lo que recupera la FEC no debe entrar en la medida del RTT ni en la ventana. kcp-go lo marca con otro tipo [F5-kcp-go A3], aunque sí genera ACK (A4).

### Qué significa para cengarde [razonado salvo cita]
- **Qué compra la FEC.** Tapa el hueco hasta que reacciona la salud de enlaces: ~1–1,5 s para declarar MUDO, 2 s para silenciar, y los cortes de menos de 1 s de Starlink. Sin FEC ni duplicación, lo que iba por un enlace que muere se pierde en ráfaga y el TCP interior lo ve. La pérdida suelta ya la cubren el ARQ de la radio y el TCP interior.
- **Forma barata.**
  - XOR (GF(2)) por grupos con a lo sumo un símbolo por enlace, y la paridad por el enlace de menor OWD.
  - El XOR va sobre la trama cengarde completa (cabecera con el byte de enlace a 0, más el payload). Así el paquete recuperado trae un MAC válido y pasa por la dedup y el anti-replay normales.
  - La paridad es un tipo de mensaje v4 nuevo, con MAC propio y numeración aparte.
  - Solo para los paquetes grandes: los chicos ya se duplican según el ROADMAP, y así se evita rellenar.
- **Expansión.** Queda en ≤1,34× solo si el reparto es igual entre enlaces. Con pesos por capacidad desiguales sube a 1,5–1,7×, salvo que se tope la parte de cada enlace.
- **Latencia.** El paquete recuperado llega con el último símbolo del grupo. Eso es hasta el spread entre los enlaces activos (≤150 ms por el silenciado), más lo que tarda en llenarse el grupo. Solo sirve si el buffer de reorden del receptor retiene al menos ese tiempo; si no, RACK (min_RTT/4) o QUIC (~RTT/8 extra) ya declararon la pérdida [resumen F9].
- **Congestión.** Una FEC en el túnel tapa la congestión al Cubic interior [F4-rfc9407-tetrys §6.1], así que cengarde tiene que frenar por enlace con su propio OWD.
- **CPU.** El XOR de 1400 B debería costar menos que el SipHash que ya se paga [MEDIR].

### Qué significa para SmoothStream
- **RLC frente a RS.** Lo medido apoya RLC para pérdidas aisladas y deja a RS mejor con ráfagas largas [F4-quic-fec-mpquic A1–A4]. No reabre la decisión.
- **Repartir RLC.** Con RLC, repartir entre patas gana menos de lo que haría pensar RS (§7.1), y la recuperación espera a la pata más lenta [F4-deadline-aware-multipath]. Las dos cosas son coherentes con la regla ya decidida del spread sobre el plazo.
- **Minimax con tope.** Coincide con Bedin (un respaldo débil es inútil) y con la cuenta 1/(1−s).
- **Gobernador T7.** Tetrys §6.2 respalda mover juntos la paridad y la tasa del encoder: más paridad con menos fuente.
- **Correlación en el estadio.** Es la incógnita que más decide: sin medirla, todo reparto supone independencia.

**Qué medir:**

- [cengarde] Spread de OWD entre los enlaces activos (p50 y p99), con los 4 enlaces 5G y Starlink cargados: fija cuándo llega el último símbolo de un grupo y cuánto tiene que retener el buffer de reorden.
- [cengarde] Fracción del tráfico que lleva el enlace más cargado con el reparto por capacidad: dice si ≤1,34× es alcanzable (1/(1−s)).
- [cengarde] Correlación de pérdidas entre los 4 operadores y Starlink en el sitio del router: pérdidas simultáneas en ventanas de 10, 50 y 100 ms, y c por par con el método de Salzburg.
- [cengarde] Forma de las ráfagas dentro de un enlace: duración en ms y en paquetes de los cortes de Starlink en los bordes de 15 s y de los handovers 5G.
- [cengarde] CPU por paquete en la Pi 4 del XOR de grupo, frente al SipHash que ya se paga, a 20, 50 y 100 Mbit/s, con los hilos por enlace apagados y encendidos.
- [cengarde] Throughput y retransmisiones espurias de TCP Cubic y BBR interiores (Linux, Android, Windows, iOS) durante la muerte de un enlace (hasta MUDO, ~1–1,5 s), en tres casos: FEC estructural, duplicación k = 2 y sin protección.
- [cengarde] Relleno real del XOR con la mezcla de tamaños del tráfico, y cuánto se ahorra protegiendo solo los paquetes grandes.
- [cengarde] Si la FEC esconde la congestión: con un enlace limitado por netem, ver si el TCP interior empuja más y sube el OWD de ese enlace.
- [SmoothStream] Correlación de pérdidas entre las 4 patas en una celda saturada (estadio): fracción del tiempo con 2 o más patas perdiendo a la vez, y c por par. Decide entre FEC repartida, menos patas o duplicar en las dos mejores.
- [SmoothStream] Clasificación de los eventos de pérdida con el log del Quectel: ver si sube antes el RTT/OWD (congestión) o si cae el SINR/RSRQ o hay handover sin subida del RTT (RF), y medir con cuánto adelanto avisa la radio.
- [SmoothStream] Ganancia real de repartir RLC entre patas frente a hacerlo por pata, con el spread medido (100–150 ms), dado que QUIC-FEC ve poca ventaja de repartir con RLC.
- [SmoothStream] Lazo T7 con congestión de celda: calidad entregada al subir la paridad bajando a la vez el bitrate del encoder (Tetrys §6.2), frente a subir solo la paridad.
- [SmoothStream] Duplicar sin FEC frente a bonding + FEC en video en movimiento (la recomendación de Peplink), con las mismas patas.
- [SmoothStream] Si en campo aparecen ráfagas largas dentro de una pata, comparar RLC con RS en ese caso (QUIC-FEC A2).

---

## ARQ en multicamino (por qué camino reintentar, plazos, relación con la FEC, HARQ tipo II)

## ARQ en multicamino

**Estado de la evidencia.** Las normas definen el mecanismo del ARQ (NACK, ventanas, plazos), pero no eligen el camino del reintento: eso queda a la implementación [F1-vsf-tr06-1-simple A3, A5] [F2-rfc8684-mptcp A9] [F2-quic-multipath A11].

- Lo medido es MPTCP o QUIC con flujos visibles (BLEST, XLINK, el preprint de 2025).
- Chuat y Garrido son simulaciones.
- Dejero es una patente.

Nada mide el ARQ multicamino con 100–150 ms de spread de RTT, ni sobre 5G o Starlink.

### 1. Qué fijan las normas
- **RIST Simple** [F1-vsf-tr06-1-simple]:
  - el NACK puede ir por cualquier conexión, o por varias (A4);
  - la retransmisión puede ir por cualquier conexión según §5.4 (A3, matizada), pero si el flujo se reparte, §5.3.3 manda (shall) usar el mismo algoritmo que los originales (A5);
  - la ventana de NACK es estática (A9);
  - valores por defecto, informativos: buffer de 1000 ms, reorden de 70 ms y 7 pedidos separados por (1000 − 70)/7 = 132 ms.
- **RIST Main** [F1-vsf-tr06-2-main]:
  - con la secuencia RTP de 16 bits, el ARQ soporta ~1 s de RTT a 100 Mbit/s, con 7 reintentos y 7 TS por RTP (A6, A7); el tope escala al revés de la tasa (razonado del supervisor);
  - Main extiende la secuencia a 32 bits (A8).
- **RIST Advanced** [F1-vsf-tr06-3-advanced]: el mismo ARQ (A7), con NACK de 32 bits (A6), y retransmisiones solo como paquetes protegidos (A10).
- **SRT** [F1-srt-draft]:
  - NAK inmediato, más un NAK periódico cada (RTT + 4·RTTVar)/2 con piso de 20 ms (A2);
  - el emisor da prioridad a la retransmisión sobre los datos nuevos (A3);
  - latencia recomendada de 3–4 RTT, con mínimo de 120 ms (A4);
  - descarte tardío a 1,25× la latencia (A5).
- **MPTCP** [F2-rfc8684-mptcp]:
  - puede reintentar por otro subflujo con el mismo DSN (A7);
  - debe reintentar también por el original, por los middleboxes (A8), algo que no aplica a un túnel UDP;
  - deja la política abierta, de agresiva a conservadora (A9).
- **QUIC multipath** [F2-quic-multipath]:
  - tiene un espacio de números de paquete y un ACK por camino (A3, A4);
  - duplicar retransmisiones es «not recommended for general purpose use» (A11);
  - tras el PTO, sugiere reintentar por otros caminos (A12).

### 2. Por qué camino reintentar
- **Por la pata más rápida o la más sana.**
  - Dejero da prioridad a los enlaces de menor latencia para lo perdido. En el caso general de datos que deben llegar, reintenta por los más fiables, a costa de retardo (A11, matizada). Reenvía de inmediato si la hora de reproducción del paquete se acerca al retardo del enlace más rápido (N1). Es patente: no prueba el producto [F7-dejero-us10028163].
  - Chuat reintenta por otro camino con un timeout por par de caminos [F4-deadline-aware-multipath A1–A3].
  - srtla reenvía por el enlace de mejor puntaje en ese momento [F5-belabox-srtla A6].
  - Es lo que ya hace libRIST en SmoothStream.
- **Por el mismo reparto.** Es lo que manda RIST §5.3.3 [F1-vsf-tr06-1-simple A5].
- **Reinyectar solo cuando el margen se agota** [F3-xlink].
  - XLINK reinyecta lo no confirmado solo cuando el bloqueo multicamino amenaza la QoE (A7).
  - Con el umbral (95, 80) logra 66 % menos muestras con menos de 50 ms de reproducción por delante, con 2,1 % de tráfico extra (A3, corregida).
  - En producción, el multicamino sin control fue peor que un solo camino: p99 hasta 28 % peor y rebuffering 34–96 % peor (A8, corregida).
- **Lo que cuesta reinyectar sin control.**
  - La penalización y reinyección de minRTT llega a 0,53 MiB de retransmisiones en 3G+WLAN (emulación, tráfico bulk), y BLEST ahorra hasta el 80 % [F3-blest].
  - Round-robin es el que más retransmite en el escenario de heterogeneidad intensa del preprint de 2025 (A7, matizada) [F3-mptcp-sched-eval-2025].
  - Raiciu reenvía por el subflujo con ventana libre el dato que retiene el borde de la ventana (A4) [F3-raiciu-nsdi12].
- **El NACK.**
  - srtla manda el NAK solo por el último enlace que entregó datos: si ese enlace cae, el pedido se pierde (A5) [F5-belabox-srtla].
  - RIST permite mandarlo por varias conexiones [F1-vsf-tr06-1-simple A4].
  - Chuat confirma por el camino de menor retardo (A2), aunque admite que un RTT preciso por camino exige confirmar por el mismo camino (§VIII-C).

### 3. Plazos y cuántos reintentos
- **Timeout por par de caminos** [F4-deadline-aware-multipath].
  - Con retardo fijo, t_i = d_i + d_min (A1). Tiene que ser lo bastante chico para entrar en el plazo y lo bastante grande para no adelantarse al ACK (A3, matizada).
  - En ns-3, con caminos de 400 y 100 ms, el margen fue de +100 ms para cubrir una desviación medida de hasta 50 ms.
  - El paper prevé 2 o 3 reintentos como máximo, porque más suelen exceder la vida del dato (A12, corregida).
- **Garrido** (simulación de un camino, d en slots) [F4-joint-sched-coding-delayed-feedback]:
  - con ARQ, el retardo crece linealmente con el retardo de realimentación d, y desde d > 40 slots (a = 0,9, p = 0,1) es peor que los esquemas predictivos; con d chico gana el ARQ (A1, corregida);
  - con 3 caminos y p = 0,2, la propuesta supera claramente a round-robin con ARQ, aunque solo en curvas (A5).
- **Descartar a tiempo.** SRT descarta a 1,25× la latencia [F1-srt-draft A5]. Dejero tira paquetes para cumplir la latencia máxima (N4). Tetrys abandona la ráfaga que no puede recuperar para salvar las siguientes [F4-rfc9407-tetrys A6].
- **Bloqueo de cabeza de línea.** El ARQ lo tiene [F4-joint-sched-coding-parallel A3].

### 4. Relación con la FEC
- **El corte k\*.** La cuenta de SmoothStream (k* = RTT × tasa de reparación) coincide con Garrido si se lee d como paquetes en vuelo, o sea d ≈ RTT × pps. El audio, con pocos pps, tiene d chico y le gana el ARQ; el video, con muchos pps, tiene d grande y le gana la codificación [razonado].
- **QUIC-FEC.** El QUIC fiable declara la pérdida a 9/8 RTT desde el envío (el «98» de la extracción es un artefacto de pdftotext). Con OWD > 45 ms, la retransmisión ya no entra en un búfer de 100 ms, y la FEC gana (A10, matizada) [F4-quic-fec-mpquic].
- **Tetrys** afirma recuperar con un retardo independiente del RTT, como tesis y sin medida (A1).
- **kcp-go.** Lo recuperado por FEC no entra en el RTT ni en la ventana del ARQ (A3), pero se confirma igual (A4) [F5-kcp-go].
- **Chuat** prefiere, por equidad, reintentar sin redundancia de código (opinión, §IX.B, comprobado).

### 5. HARQ tipo II: paridad nueva en lugar de retransmitir
- **Tetrys** es lo único con un diseño completo [F4-rfc9407-tetrys]:
  - con feedback, los símbolos recibidos o decodificados salen de la ventana, así que la paridad nueva cubre solo lo no confirmado (A3, matizada);
  - la generación se dispara al llegar una fuente o un feedback (A4);
  - el feedback es opcional, y perderlo solo encarece (A5);
  - los coeficientes son Vandermonde deterministas (N1).
- **Garrido** decide si mandar un paquete de información o uno codificado según una realimentación retardada. Pierde menos del 5 % de capacidad (A2) y no gasta redundancia si d < 1/p (A3); es simulación.
- **Nadie mide HARQ II frente a la retransmisión.**
- **Cómo encaja en RLC** [razonado]:
  - cualquier reparación nueva sobre una ventana que contiene m huecos es una ecuación útil para cualquiera de ellos;
  - ante un NACK, m reparaciones nuevas cuestan lo mismo que m retransmisiones;
  - no importa cuál de ellas se pierda: otra nueva sirve igual;
  - pueden ir por la pata sana sin rastrear qué reintento falló.

  Límites: el hueco tiene que seguir dentro de la ventana de codificación, y el receptor paga la decodificación.

### 6. Debajo de TCP y QUIC (el caso de cengarde)
- **Lo que tolera el tráfico interior** [resumen F9] [F9-rfc9002-quic-loss] [F9-linux-ip-sysctl A6]:
  - RACK arranca con una ventana de reorden de min_RTT/4, que crece con DSACK hasta el SRTT;
  - QUIC declara la pérdida a 9/8 del RTT desde el envío, o sea ~1/8 RTT de margen;
  - Linux trae TLP activo por defecto (tcp_early_retrans = 3).
- **Tras un cambio de ruta que reordena,** Cubic se recupera en menos de 0,5 s, mientras Reno y BBR siguen retransmitiendo más de 1 s (emulación LEO) [F8-leo-reordering A5].
- **La radio celular ya retransmite en capa 2,** y eso exige buffers grandes [F8-bufferbloat-3g4g A10].
- **La cuenta** [razonado]:
  - un ARQ del túnel tarda al menos detección + OWD del NACK + OWD del reenvío, o sea ≥1 RTT del túnel más el spread;
  - el TCP interior tolera ~RTT_interior/4 extra, y QUIC ~RTT_interior/8;
  - con el VPS en Santiago, el RTT interior a destinos cercanos es casi el del túnel, así que el reintento del túnel llega después de que el emisor interior ya reintentó;
  - solo gana con destinos lejanos, o con UDP sin recuperación propia y con un jitter buffer holgado.

### Qué significa para cengarde [razonado salvo cita]
- **Prioridad.** El ARQ opcional del ROADMAP («1–2 enlaces más recuperación por el mejor») va detrás de la FEC liviana y de la duplicación selectiva: para tráfico interactivo cercano, casi nunca llega a tiempo.
- **Si se hace, barato:**
  - reenviar la trama original guardada: la cabecera y el MAC no cambian, solo el byte de enlace, que está fuera del MAC; no hay costo criptográfico y la dedup tira los duplicados;
  - un anillo preasignado en el emisor, de ~1,9 MB para 150 ms a 100 Mbit/s;
  - el NACK como mensaje v4 con MAC, por los dos enlaces de menor OWD;
  - el reenvío por el enlace de menor OWD;
  - un solo intento, con plazo igual al hold del buffer de reorden.
- **Detectar la pérdida.** Con la secuencia global compartida, un hueco es pérdida cuando todos los enlaces activos ya entregaron un número mayor. Eso supone que cada enlace es FIFO, y Starlink puede reordenar dentro de un enlace en un cambio de ruta [F8-leo-reordering] [MEDIR]. Para detectarla antes hace falta un contador por enlace, como los espacios por camino de QUIC multipath [F2-quic-multipath A3].
- **Anti-replay.** El reintento tardío tiene que caer dentro de la ventana de cengarde (~8128 paquetes) y de la de WireGuard (8192). A 100 Mbit/s con paquetes de 1400 B (~8900 pps) son ~0,9 s.

### Qué significa para SmoothStream
- **Lo ya decidido.** El NACK por la pata que reveló el hueco, el reenvío desviado a una pata sana y fec-nack-delay coinciden con Dejero, Chuat y QUIC multipath. El desvío no sigue el «shall» de RIST §5.3.3: lo anoto como contradicción, no como recomendación.
- **Riesgo.** Si la pata que reveló el hueco está en un fade, el NACK puede perderse, como en srtla. RIST permite el NACK por varias conexiones, y duplicarlo por las dos mejores patas cuesta poco [razonado; MEDIR antes de cambiar nada].
- **Reintentos.** El tope de 2–3 de Chuat y el descarte por plazo de SRT y Dejero son coherentes con el presupuesto n_arq × RTT.
- **HARQ II.** Encaja con fec-rlc dentro de libRIST: el NACK dispararía reparaciones RLC nuevas por una pata sana. Falta un banco.
- **Gobernador.** Lo recuperado por FEC cuenta como «presión», nunca como muestra de RTT [F5-kcp-go A3].

**Qué medir:**

- [cengarde] RTT del túnel por enlace (router–VPS en Santiago) frente al RTT interior a destinos típicos: si el RTT detrás del VPS es mucho menor que el del túnel, el ARQ del túnel llega tarde para RACK y QUIC.
- [cengarde] Retransmisiones espurias y DSACK del TCP interior, y pérdidas declaradas por QUIC, con bonding y con o sin ARQ del túnel, en Linux, Android, Windows e iOS.
- [cengarde] Si cada enlace es FIFO, sin reorden interno, sobre todo Starlink en cambios de ruta. Es la condición para declarar la pérdida cuando todos los enlaces activos pasaron el número.
- [cengarde] Tasa de pérdida de NACK por enlace, y costo del anillo de reenvío en la Pi (memoria y CPU).
- [cengarde] Si el kernel descarta reintentos tardíos por la ventana anti-replay de WireGuard (8192) a la tasa máxima.
- [SmoothStream] Pérdida de NACK en la pata que reveló el hueco durante un fade, y si duplicar el NACK por las dos mejores patas sube la recuperación.
- [SmoothStream] Éxito del reenvío según la pata elegida (sana frente a original), y tiempo hasta la recuperación.
- [SmoothStream] Validar el corte k* = RTT × tasa de reparación con el RTT real de cada pata, en audio y en video.
- [SmoothStream] HARQ II frente a retransmisión: fracción recuperada y bytes gastados, en banco con ráfagas netem y en campo.
- [SmoothStream] Cuántos reintentos (n_arq) entran en el presupuesto de cada perfil, y el valor de fec-nack-delay frente al cierre de la ventana RLC.

---

## tasa: control de tasa por enlace, pacing y adaptación del bitrate de la fuente a la capacidad total

## Eje: control de tasa por enlace, pacing y adaptación de la fuente a la capacidad total

**En corto.** Ninguna fuente trae un controlador de tasa por enlace listo para usar en un túnel que no ve los flujos. Sí hay cuatro piezas en las que las fuentes coinciden:
1. En celular y en Starlink la cola de cada enlace se llena sin que la pérdida avise. La señal útil de congestión es el retardo de cada enlace.
2. El ritmo de envío se fija algo por debajo de la capacidad estimada, con margen para retransmisiones.
3. La capacidad estimada desde el receptor tiende a sobrestimarse y hay que filtrarla.
4. Adaptar el bitrate de la fuente queda fuera del bonding. Se alimenta de la capacidad total, del backlog y del plazo de entrega.

Casi todo lo cuantitativo es de TCP/MPTCP entre 2012 y 2022. El lazo que adapta la fuente solo lo describen una patente y algunos vendedores.

### 1. El problema: la cola de cada enlace
- **Celular.** El bufferbloat anula el control de congestión por pérdida: el emisor sigue creciendo porque no ve pérdidas [F8-bufferbloat-3g4g A1]. Se midió en 3G/4G en 2012, con cuatro operadores de EE. UU. y uno de Corea. Hubo latencias de hasta 10 s en EVDO (pico de 3,1 Mbit/s, RTT mínimo de 150 ms, BDP ~58 KB, cwnd de más de 600 KB) [F8-bufferbloat-3g4g, números]. Para 5G no hay cifra de cola en las fichas de este eje.
- **Starlink.** En reposo, el RTT mediano a anclas locales es de 46–52 ms. Bajo transferencia masiva HTTP/3, la mediana sube a 95 ms bajando y 104 ms subiendo, con p99 de 210 y 310 ms [F8-first-look-starlink, corrección del supervisor; comprobado en el texto, l.240]. La subida medida con Ookla tiene mediana de 17 Mbit/s, máximo de 64, y menos del 5 % pasa de 30 (un terminal, Europa, abril de 2022) [F8-first-look-starlink A8]. El throughput cae en los bordes de los intervalos de 15 s y es casi constante dentro de cada intervalo [F8-mohan-starlink-multifaceted A3]. La magnitud de esa caída solo aparece en una figura que no se extrajo.
- **Modelos.** Mandar más por un enlace sin control de cola sube dos cosas a la vez: la probabilidad de entregar a tiempo y la cola que retrasa el bloque siguiente. Esos enlaces solo descartan cuando su buffer se desborda [F4-joint-sched-coding-parallel A9, A10] (modelo MDP, no medida). La pérdida y la latencia crecen con la utilización, así que un camino se comporta distinto según cuánto se le mande [F4-deadline-aware-multipath A10].
- **Tamaño de cola.** El tamaño de la cola cambia qué reparto rinde. Con colas grandes (1000 paquetes) y anchos distintos, minRTT deja de usar los dos caminos [F3-cech-thesis, corrección A1].

### 2. Dónde se puede controlar la cola
La gestión de cola tiene que estar lo más cerca posible de la cola real. El driver y el hardware tienen colas propias, y a tasa alta hace falta una cola pequeña y estable entre el algoritmo y el cable, que en Linux es BQL [F9-rfc8290-fq-codel A6, A7]. En 2018 BQL solo existía en drivers Ethernet PCIe [F3-qaware A6, matizada: la fuente no habla de USB].

[razonado] En cengarde la cola que manda está en el módem/CPE y en la celda de subida, fuera de la Pi. La única palanca es enviar por cada enlace algo menos de lo que ese enlace drena. Así la cola se forma en el anillo del hilo de ese enlace, donde se ve y se puede descartar. Con cuatro VLAN sobre un mismo adaptador USB, la cola del kernel bajo las VLAN puede ser compartida: un enlace saturado retrasaría a los demás antes de llegar al módem [MEDIR].

### 3. Señales de congestión por enlace
**Retardo sobre el mínimo.** Las fuentes usan tres escalas distintas:
- **DRWA.** La ventana anunciada vale λ·RTTmin/RTTactual·cwnd_estimada, con λ = 3 (RTT objetivo = 3×RTTmin) y suavizado 7/8. El valor de λ salió de un barrido empírico en 3G/4G de 2012, y hacerlo adaptativo quedó como trabajo futuro [F8-bufferbloat-3g4g A3, A4, A5]. El +51 % de throughput es el máximo, en Sprint EVDO con la latencia más alta; en Verizon LTE la mejora va de −1 % a +39 % [A8, matizada].
- **Peplink DWB** (vendedor). Un WAN se considera congestionado cuando su latencia pasa 2× la de reposo (30→60 ms; perfil Low 45 ms, High 75 ms), o cuando supera una latencia de corte fija por WAN [F6-peplink-speedfusion-whitepaper, corrección de números; comprobado en el texto, l.787-792].
- **CoDel.** Objetivo de 5 ms, que debe ser al menos el tiempo de un MTU a la tasa del enlace (~15 ms a 1 Mbit/s), e intervalo del orden del peor RTT (100 ms por defecto) [F9-rfc8290-fq-codel; resumen F9].

Estas señales tienen límites:
- El control por retardo (Vegas) baja mucho el RTT pero pierde throughput en celular [F8-bufferbloat-3g4g A6].
- Detrás de un AQM, los algoritmos por retardo vuelven a comportarse como si fueran por pérdida [F9-rfc8290-fq-codel A8].
- En Starlink la latencia sube en escalones cada 15 s aunque el terminal esté muy por debajo de su capacidad [F8-starlink-scheduler-constellations, corrección A2]. [razonado] Un mínimo aprendido en un intervalo no vale en el siguiente, y el escalón se leería como congestión.

**Pérdida.**
- BBR tolera hasta 2 % de pérdida por RTT al sondear el ancho antes de tratarla como congestión, y entonces reduce ×0,7 [F9-bbr-draft A5]. Es un borrador expirado, sin estatus en la IETF.
- La patente de Dejero baja la tasa de un enlace cuando sube su tasa de fallos de entrega, y lo interpreta como RF inestable [F7-dejero-us10028163 A6]. Solo lo respalda la patente.
- Peplink trata la pérdida como congestión por defecto, y para Starlink recomienda ignorarla y añadir FEC [F6-peplink-speedfusion-whitepaper A10] (vendedor).
- Si un subflujo tiene más de 10 % de pérdida, el control de congestión sobrestima su tasa. En simulación, MPTCP queda 20–30 % por debajo de TCP [F3-raiciu-nsdi12 A11].

[razonado] La pérdida por sí sola no separa un problema de RF de uno de congestión.

**Agregación celular.** BBR suma al cwnd la agregación de ACK que mide (batching y slotting en la capa 2 de wifi, celular y DOCSIS), tomando el máximo de los últimos 10 RTT. Así sigue enviando en los silencios entre ACK [F9-bbr-draft A1, A2]. [razonado] Un estimador de tasa con ventanas cortas en el receptor vería las ráfagas en que la celda concede capacidad y sobrestimaría. Hace falta una ventana larga o un máximo filtrado.

### 4. Estimar la capacidad de cada enlace
- **SRT.** La capacidad que estima el receptor puede sobrestimar mucho la real. SRT la corrige con el ancho registrado en el momento de la pérdida [F1-srt-draft, corrección A7; comprobado en el texto, §5.2.1].
- **srtla.** Cada enlace tiene una ventana en milipaquetes. Por cada secuencia reconocida crece +0,029 en el enlace que llevó el paquete, si estaba lleno, y +0,001 en todos los enlaces vivos; el tope es 60 paquetes. Reparte según ventana/en-vuelo y no aplica control de congestión [F5-belabox-srtla, corrección A2; A9]. No usa el RTT.
- **srtla-send-rs.** Calcula un target_bps por enlace con una máquina de estados (EWMA de RTT, varianza, pérdida y bitrate). Su CHANGELOG dice que todavía no se aplica como tope: solo influye el estado BackingOff [F5-srtla-send-rs, mecanismos; comprobado en CHANGELOG.md]. El reparto elige la mayor ventana/(en-vuelo+1) y multiplica por 0,7 a los enlaces con ráfagas de NAK (5 o más en 1 s) [corrección A12].
- **Dejero.** Compara lo que envía cada interfaz con lo que el servidor dice haber recibido. Un cociente de ~90 % basta para una conexión sana; si baja, esa interfaz reduce su tasa [F7-dejero-us10028163, texto l.1203]. Comprobé la cita en esta síntesis; el supervisor no la revisó, y solo la respalda la patente.
- **Plazos.** El ancho de cada camino puede tomarse de la salida del control de congestión, por ejemplo PCC [F4-deadline-aware-multipath A8].

[razonado] Para cengarde lo viable es que la respuesta de sonda (ya cada 100 ms con tráfico) lleve, autenticados, los bytes de paquetes verificados recibidos por cada enlace. Con eso el emisor calcula la tasa entregada y el cociente enviado/recibido de cada enlace. La tasa entregada solo revela la capacidad cuando el enlace es el cuello de botella:
- En modo redundancia todos los enlaces llevan todo, así que esa prueba sale gratis.
- En bonding hay que sondear de vez en cuando por encima de la estimación, pagando algo de cola.

### 5. Pacing
- **SRT.** MAX_BW fija el intervalo mínimo entre paquetes. El margen de sobrecarga deja huecos para intercalar retransmisiones sin alterar el ritmo de los datos nuevos [F1-srt-draft A12].
- **BBR.** Hace pacing 1 % por debajo del ancho estimado y deja un headroom de 0,85 sobre inflight_hi [F9-bbr-draft, números].
- **RIST.** Recomienda limitar las retransmisiones y evitar ráfagas de NACK; es texto informativo [F1-vsf-tr06-1-simple A13].
- **Sin pacing.** Los mensajes de 25 kB de quiche se apilan en los buffers de subida de Starlink y suben el RTT [F8-first-look-starlink A3, matizada: solo en la prueba de mensajes].
- **Lo opuesto.** [código, leído en esta síntesis] srtla-send-rs agrupa envíos (umbral de 4, 16 o 32 paquetes según el bitrate, vaciado cada 15 ms) para ahorrar CPU. Es lo contrario del pacing: cuesta hasta 15 ms y produce ráfagas.

### 6. Control de congestión multicamino en las normas
- **Estado por camino.** Cada camino lleva su propio estado de congestión y no envía más de lo que su ventana permite. Es MUST en MP-DCCP [F2-mp-dccp A9] y está también en QUIC multipath [F2-quic-multipath A8].
- **Acoplamiento.** Solo si los caminos comparten cuello de botella se acopla el aumento al estilo LIA (RFC 6356) [F2-quic-multipath A8; F2-rfc8684-mptcp A3].
- **Otras normas.** TR-348 pide límites de tasa por sesión, por camino y por clase [F2-bbf-tr348 A11]. MPRTP deja fuera el control de congestión y el balanceo [F2-mprtp A2]. En 3GPP, el modo LBPAO deja que el teléfono fije sus propios porcentajes para maximizar el agregado de subida [F2-3gpp-ts24193-atsss A7].
- **Peso relativo.** Con heterogeneidad intensa entre caminos pesa más el control de congestión que el planificador [F3-mptcp-sched-eval-2025 A3] (Mininet, 2×100 Mbit/s, solo tráfico bulk).
- **BALIA.** El control acoplado BALIA dio +18 % de throughput y −17 % de retraso de cola a alta movilidad, pero no está claro contra qué se compara [F8-dual-lte-mptcp A10, matizada].
- **OMR y KCP.** OpenMPTCProuter configura bbr2, pero lo migra a BBRv3 en cualquier kernel que no sea 5.4 [F5-openmptcprouter A3 matizada, N2]. KCP permite apagar su control de congestión por sesión [F5-kcp-go A7].

### 7. Adaptar la fuente a la capacidad total
- **El transporte no lo hace.** srtla lo deja a la aplicación, que debería ajustar su bitrate según la ocupación del buffer de envío SRT (SRTO_SNDDATA) o según el RTT. El README avisa además que con srtla esos valores salen algo más altos aunque no haya congestión, por el reordenamiento [F5-belabox-srtla A9; README comprobado]. MPRTP lo deja fuera, y su evaluación usa una tasa de codificación fija [F2-mprtp A2, A10 matizada].
- **Lazo de Dejero (solo patente).**
  - El encoder codifica según la tasa total de todas las conexiones. Si cambia el ancho de una, revisa si eso afecta al total [F7-dejero-us10028163 A8, matizada: mezcla dos variantes de la patente].
  - El backlog más los reenvíos se convierten en tasa dividiéndolos por el tiempo que queda hasta el plazo glass-to-glass. Si esa suma pasa el total disponible, se ajusta la codificación [N2].
  - Un backlog por sí solo no obliga a bajar el bitrate, porque el buffer del receptor da tiempo a vaciarlo [N3].
  - Para bajar, el encoder reduce el bitrate, la resolución o los cuadros por segundo [A7].
- **Vendedores.** LiveU y Haivision dicen que adaptan el bitrate, sin explicar cómo [F7-liveu-lrt A2; F7-haivision-sst A2]. Solo es palabra del vendedor.
- **Granularidad de la decisión.**
  - Con ABR (fastMPC) sobre trazas mmWave emuladas, chunks de 1 s dan 33,6 % (29,8 %) menos stall y 21,5 % (35,9 %) más bitrate que chunks de 2 s (4 s) [F8-variegated-5g A6, matizada].
  - Los ABR existentes sufren 3,7–259,5 % más stall con trazas 5G que con trazas LTE; BBA no empeora [A5, matizada].
  - En dual-LTE en la calle, el tráfico más constante (segmentos de 1 s) fue el que más cayó a alta velocidad: −49 % de throughput, con enlaces de 8 Mbit/s [F8-dual-lte-mptcp, números].
- **FEC dentro de la capacidad.** La tasa de código puede adaptarse cuando la pérdida y el retardo varían mucho. Si la FEC va dentro del transporte, la cantidad de reparación se calcula junto al control de congestión para no causar congestión [F4-rfc9407-tetrys A7, A8, matizadas].

### Qué significa para cada producto
**cengarde** [razonado]. cengarde no controla la fuente, que es el TCP/QUIC de adentro. Su trabajo es doble:
- (a) No meter por ningún enlace más de lo que ese enlace drena, para que la cola quede en la Pi y no en el módem.
- (b) Dejar que el control de congestión interior vea el retardo o la pérdida, sin esconderlos.

Diseño mínimo, en funciones puras:
- Un estimador por enlace alimentado por los bytes verificados recibidos (en la respuesta de sonda), el OWD mínimo y el actual, la profundidad del anillo y los EAGAIN.
- Congestión cuando OWD − OWD_mín supera un umbral durante un intervalo. Empezar entre 2× y 3× la base, o entre +5 y +15 ms, y MEDIR. El umbral tiene que quedar por debajo del de silenciado (150 ms detrás del más rápido), para frenar antes de silenciar.
- Las tasas estimadas son los pesos de capacidad del WRR de `cg_sched_mask`.
- Pacing solo en los enlaces que están cerca de su tope.

En modo redundancia no cambia nada: el silenciado ya actúa como control grueso. Ojo con una FEC liviana: si repara descartes de congestión, el TCP interior no frena [F4-rfc9407-tetrys A12].

**SmoothStream** [razonado]. El gobernador T7 fija el bitrate del encoder así: capacidad total × (1 − margen), menos la paridad y el presupuesto de ARQ.
- **Ataque rápido** cuando el backlog, convertido a tasa con el plazo, supera el total (la forma de Dejero).
- **Gate** para el backlog pasajero que el buffer del receptor absorbe.
- **Release lento**, más largo que el ciclo de 15 s si una pata es Starlink.
- **Peso y perfil FEC se mueven juntos**, porque la paridad come capacidad de la pata.

Regla candidata para separar causas:
- Si el retardo sube con la carga, es congestión: bajar peso y bitrate, no poner más FEC.
- Si hay pérdida sin subida de retardo y con caída de SINR, es RF: más FEC.
- Un escalón cada 15 s en Starlink no es ninguna de las dos.

Ninguna fuente valida esa regla. La forma del lazo descansa en una patente y en razonamiento, y hay que medirla en fierro.

**Qué medir:**

- Para cada enlace (WOM, Entel, Claro, Movistar, Starlink) y en ambos sentidos: curva de OWD frente a la tasa ofrecida desde la Pi al VPS, para ubicar el codo y la cola máxima en ms del módem o la celda (el bufferbloat real).
- Variación de la capacidad de cada enlace a 100 ms, 1 s y 15 s, y si un máximo filtrado de ~10 RTT sigue a la capacidad real sin sobrestimar en las caídas.
- Si la cola del adaptador USB con 4 VLAN es compartida: saturar un enlace y medir el OWD de los demás.
- Exactitud de la tasa entregada, estimada con los bytes verificados por enlace que lleva la respuesta de sonda, frente a iperf3 en cada enlace.
- Umbral de congestión por OWD (2× y 3× la base, +5 ms, +15 ms): goodput y RTT del TCP interior (Cubic y BBR; Linux, Android y Windows) a través del túnel en bonding.
- Escalones de 15 s de Starlink: su magnitud en OWD y en throughput, y si disparan el umbral de congestión o el silenciado.
- CPU de la Pi 4 con pacing por enlace a 20, 50 y 100 Mbit/s, con un solo hilo y con un hilo por enlace.
- Si una FEC liviana bajo WireGuard enmascara las pérdidas de congestión: RTT y cola del TCP interior con y sin FEC.
- SQM/cake sobre las VLAN 5G en OpenWrt: si limita el agregado o pelea con el pacing de cengarde.
- SmoothStream: constantes de ataque y release del gobernador frente a fades y caídas medidos; ocupación de la cola de envío y retransmisión de libRIST como señal; cuánta capacidad de cada pata comen la paridad y el ARQ.
- SmoothStream: cuánto se adelantan de verdad el SINR y el RSRP de los Quectel a la caída de capacidad, y si en la congestión del estadio el retardo de cada pata sube antes que la pérdida.

---

## fallos: fallos de campo documentados y sus arreglos

## Eje: fallos de campo documentados y sus arreglos

**En corto.** Hay poca evidencia de campo de verdad:
- el A/B de producción de XLINK (Taobao);
- reportes de usuarios de OpenMPTCProuter (foro, leídos vía WebFetch);
- mediciones de Starlink y de LTE/5G en la calle;
- el dataset de Salzburgo;
- los arreglos que dejan los repos (srtla, srtla-send-rs, MLVPN, kcp-go).

El resto son modos de falla que describen normas y papers. Tres lecciones se repiten:
1. Juntar enlaces heterogéneos sin control puede quedar peor que el mejor enlace solo.
2. Starlink y 5G tienen eventos de menos de un segundo a varios segundos que no son congestión.
3. Muchas fallas son de implementación: contadores que dan la vuelta, errores pasajeros tomados como caída, umbrales apagados por defecto y documentación que no coincide con el código.

### 1. Juntar enlaces puede empeorar
- **XLINK, producción** (Taobao, una semana, video en Android). El QUIC multicamino sin control (vanilla-MP) empeoró el rebuffering más de 34 %, y hasta 96 %, frente a un solo camino. También empeoró el tiempo de petición: en mediana y p90 los días 1, 3, 4 y 5, y en p99 siempre, hasta 28 % [F3-xlink N1, A8 matizada].
  - Arreglo: duplicar (reinyectar) solo cuando el margen de reproducción se acorta, y priorizar el primer cuadro.
  - Resultado frente a un camino: −23,8 a −67,6 % de rebuffering y 19–50 % mejor p99 de petición, con 2,1 % de tráfico extra en el umbral (95, 80) [F3-xlink, números y corrección A3].
  - Sin acelerar el primer cuadro, su p99 fue 14 % peor que con un camino; con la aceleración, más de 32 % mejor [A9].
- **OpenMPTCProuter, campo** (Raspberry Pi 4, LTE de T-Mobile + Starlink, noviembre de 2023). El agregado (~34,0/30,1 Mbit/s) quedó por debajo del enlace más lento (LTE 141,6/30,2; Starlink 166,4/138,4; las cifras varían mucho entre corridas).
  - El mantenedor respondió «Yes there is a problem here» y sugirió desactivar SQM y probar Cubic en vez de BBRv2, sin dar una causa [F5-openmptcprouter A8, matizada; foro].
  - Con Starlink + 2 LTE el túnel caía bajo carga. El planificador redundant lo estabilizó, pero sin sumar ancho [A7, foro].
  - La rama 5.4 es legacy y no recibe arreglos [A9].
  - Los reportes de MPTCP fallando con kernel 6.x no son verificables y no se usan [A10].
- **Dual-LTE en la calle.** A más de 60 km/h las descargas MPTCP tardan más del doble que MPTCP parado, y rinden peor que un TCP simple la mayor parte del tiempo. En la medida controlada empatan en auto (31,9 frente a 31,7 Mbit/s) [F8-dual-lte-mptcp A6, A11, matizadas]. Un handover produce hasta 8× de diferencia de RTT entre subflujos y −74 % de throughput en ambos caminos [A4].
- **Subflujo lento.** Un subflujo lento y con pérdida se vuelve el cuello de toda la conexión [F4-fmtcp A1] (solo el resumen). Con timeouts frecuentes en el mejor subflujo (más de 10 % de pérdida), MPTCP queda 20–30 % por debajo de TCP porque el control de congestión sobrestima ese subflujo (simulación) [F3-raiciu-nsdi12 A11].
- **Cambio de camino.** Mover tráfico entre caminos durante la vida de un flujo produce un salto de rendimiento. El bypass del tráfico de tiempo real se justifica aparte, por el retardo de reordenamiento [F2-bbf-tr348 A12, matizada]. Además, el túnel iguala su latencia a la del enlace más lento, y eso retrasa detectar la pérdida en el rápido [F6-peplink-speedfusion-whitepaper, mecanismos; vendedor].

[razonado] Arreglo común:
- Una guarda de «no peor que el mejor enlace solo», que vuelva a un enlace o a duplicar cuando el bonding no lo supera.
- Duplicar solo cuando falta margen.

### 2. Starlink
- **Ciclo de 15 s.** Starlink reconfigura su asignación cada 15 s, de forma global:
  - El throughput cae en los bordes de cada intervalo [F8-mohan-starlink-multifaceted A3].
  - Hay cortes breves de menos de un segundo; los autores extrapolan que se notarán más con más suscriptores [A6].
  - En cloud gaming (Amazon Luna) hay caídas a menos de 20 FPS que coinciden con los bordes y solo se ven a escala de fracciones de segundo; en Zoom el FPS no cambia [A7, matizada].
  - Con la vista obstruida (un terminal), el fin de las ventanas de conectividad cae en los bordes [A5, matizada].
  - La latencia sube en escalones cada 15 s aun muy por debajo de la capacidad [F8-starlink-scheduler-constellations, corrección A2]. El modelo de esos autores no conoce la carga ni la densidad de terminales [A8].
- **Cortes largos.** En un terminal de 2022 hubo eventos de más de 1 s y ráfagas de más de 100 paquetes perdidos [F8-first-look-starlink A6, matizada: que una FEC corta no los repare es inferencia de la ficha]. En otro terminal no hubo cortes de segundos [F8-mohan-starlink-multifaceted A11]. No hay PEP en el camino; solo NAT que tocan los checksums [F8-first-look-starlink A10].
- **Cambios de ruta LEO** (emulación). Reno sufre mucho. Cubic y BBR quedan cerca de la capacidad en promedio, pero con caídas periódicas y significativas justo después de algunos cambios [F8-leo-reordering A6, A7].
- **Peplink** (vendedor, de oídas). Habla de cambio de satélite «e.g., every 15–20 seconds» con fluctuaciones. Dice que con dos antenas DWB tiene dificultad para priorizar, y recomienda ignorar la pérdida y añadir FEC [F6-peplink-speedfusion-whitepaper A10, matizada].

[razonado] Frente a la salud ya hecha en cengarde:
- Un corte de menos de un segundo no llega a silenciar, porque hacen falta 2 s de atraso.
- Uno de más de 1 s sí puede marcar el enlace como MUDO (~1–1,5 s).
- Si la espera para volver se duplica, un patrón que se repite cada 15 s podría dejar a Starlink fuera más tiempo del que dura cada corte [MEDIR].

### 3. Celular y 5G
- **Bufferbloat.** Latencias de hasta 10 s (EVDO, 2012). Los topes fijos de ventana de los teléfonos mitigan, pero no bastan [F8-bufferbloat-3g4g A1, A2]. En 2012 había pocos AQM desplegados [A11, matizada: no vale como dato de los operadores chilenos de hoy].
- **Handovers** (T-Mobile n71, ruta de 10 km en auto, un teléfono). SA tuvo 13 handovers y NSA+LTE 110. En NSA hubo ~90 verticales (4G↔5G) contra 13–20 horizontales [F8-variegated-5g A2, A3].
  - La mayoría de los handovers 5G→LTE son fallos de 5G, sobre todo SCG failure [F8-handover-5g-2025 A7].
  - Con configuraciones agresivas, hasta 25 % son ping-pong (EE. UU., 2022–2024) [A8].
- **TCP de una conexión.** Quedó limitado a 500 Mbit/s con los buffers por defecto (kernel 4.18, mmWave, servidor en Azure) [F8-variegated-5g A8].
- **Timers de cola RRC.** 10 s en T-Mobile SA, similar a T-Mobile NSA y Verizon NSA. Otro estudio citado vio 20 s en 5G, el doble de los 10 s de 4G [F8-variegated-5g, texto comprobado en esta síntesis]. [razonado] Las sondas de 1 s en reposo mantienen el enlace conectado.
- **Correlación entre operadores** (autopista de Austria, coche, 2 operadores, 2021–2022). Se cuenta como perdido lo que no llega en 100 ms.
  - Pérdida: 0,86 % y 2,56 % por separado; 0,022 % si fueran independientes; 0,077 % real, o sea 3,5× más.
  - Caída a 1 Mbit/s: 0,017 % si fueran independientes frente a 0,071 % real, 4,2× más [F8-salzburg-multiprovider, tablas comprobadas].
  - Es una muestra de conveniencia y los autores llaman preliminar a su evaluación [A6].

### 4. Fallas de implementación y sus arreglos (código)
- **Vuelta del número de secuencia.** srtla-send-rs tomaba por duplicado el ACK que pasa de 0x7fff_ffff a 0, y eso congelaba el RTT y el en-vuelo. Se arregló comparando en aritmética serial [F5-srtla-send-rs A8]. srtla tiene un TODO que reconoce que la vuelta no se maneja [F5-belabox-srtla, mecanismos].
- **Error pasajero tomado como caída.** En srtla, un sendto() corto o fallido saca el enlace de inmediato y, al vencer el timeout, la ventana vuelve a 1 paquete. Como el socket es no bloqueante, un EAGAIN podría sacar un enlace por un evento pasajero; es inferencia, y con SO_SNDBUF de 8 MiB es improbable [F5-belabox-srtla A7]. Los timeouts son 4 s en el emisor y 10 s en el receptor.
- **Reconexión.** srtla-send-rs reintenta con retroceso: 5, 10, 20, 40, 80 y 120 s [F5-srtla-send-rs A10].
- **Sondas que desvían tráfico.** La exploración de enlaces de srtla-send-rs desviaba cerca de la mitad del tráfico. Reescrita como un paquete por enlace hambriento cada 200 ms, no movió la entrega (0,76 puntos, Welch t=0,75, n=15, netem, un solo escenario), y se eliminó [F5-srtla-send-rs A9, matizada].
- **La documentación no coincide con el código** (MLVPN):
  - El manual dice que el timeout de reorden es SRTT×2; el código usa 2,2×(srtt+4·rttvar) [F5-mlvpn A11].
  - Keepalive: timeout/2 en el manual, 1 s en el código.
  - loss_tolerence vale 0 en el manual y 100 en el código, lo que deja el estado LOSSY casi apagado: solo se activa con 64 de 64 paquetes perdidos [corrección A7].
  - Un enlace DISCONNECTED no cuenta para entrar en fallback [corrección A10].
  - Al vencer el timer de reorden se vacía todo en orden de llegada y se resetea el buffer [corrección A2].
- **La documentación no coincide con el código** (srtla-send-rs):
  - El README dice que el siguiente eco limpia el estancamiento; el código exige una racha de 2× la ventana.
  - El CHANGELOG habla de un goteo a los enlaces compuertados; el código los salta del todo porque «a trickle would only add latency» [resumen F5; comprobado en enhanced.rs].
- **kcp-go.**
  - Si falla la paridad, cuenta el error y mantiene el seqid creciente para que el receptor siga viendo los huecos [F5-kcp-go A11].
  - Un umbral fijo de 500 ms no coincide con el comentario, que remite a un rto() que no existe [mecanismos].
  - Los paquetes FEC no actualizan el RTT [código kcp.go, leído en esta síntesis].
- **Planificadores de investigación.** DAPS y MuSher provocan kernel panics [F3-cech-thesis A10]. QAware se comparó con ECF, BLEST y DAPS en otra versión de MPTCP (v0.93 frente a v0.89) [F3-qaware N3, A10 matizada]. Mininet dentro de una VM dio resultados no deterministas [F3-mptcp-sched-eval-2025 A11], y todo fue tráfico bulk [A12]. Meta-DAMS reporta hasta 35 % menos stalling, pero en emulación, con 100 corridas por esquema [F7-meta-dams A8, A9, A12].

### 5. Modos de falla que fijan normas y papers
- **SRT.**
  - Conmutar a backup suelta una ráfaga de todo lo no confirmado y, con ancho limitado, el reenvío puede no llegar a tiempo. El documento no sabe qué recomendar [F1-srt-socket-groups A11].
  - El grupo balancing no detecta rápido un enlace roto [A13].
  - Un receptor que salta un paquete tardío manda un ACK falso y el emisor nunca se entera: hay que registrar los saltos [F1-srt-draft A9].
  - Apagar el descarte tardío puede pausar la entrega sin límite: es mejor subir la latencia [A10].
- **RIST Advanced.** Con la interoperabilidad opcional de §9, se queda en Main si el par no anuncia el bit [F1-vsf-tr06-3-advanced A3, matizada].
- **MPTCP.** Tras un fallback con mapeo infinito solo un subflujo envía, y la conexión no vuelve a MPTCP [F2-rfc8684-mptcp A11, A12]. Si el SYN+ACK vuelve sin opciones MPTCP, la conexión baja a TCP de un camino [F2-linux-mptcp-doc A4, matizada]. Si cae un camino, el tráfico se reinyecta en los otros [A3].
- **Control sin autenticar.**
  - En RFC 8157 los mensajes de control se pueden falsificar; la protección es una clave GRE de 32 bits en claro más la IP de origen [F2-rfc8157-gre-bonding A11, matizada].
  - En Tetrys el feedback falso es un DoS, y hace falta autenticación por debajo [F4-rfc9407-tetrys A10].
  - Descartar feedback no rompe Tetrys, pero sube el cómputo en ambos lados [A11]. La ventana MUST tener tope [A13].
  - Una FEC por debajo del transporte oculta las pérdidas de congestión [A12].
- **Esquema FEC.** Si las puntas usan esquemas distintos (GF(2) frente a GF(2^8)), la decodificación falla o entrega símbolos corruptos. Está en la sección de seguridad y se lee por analogía como contrato entre puntas [F4-rfc8681-rlc A10, matizada].
- **Reglas que se saltan en silencio.** Una regla ATSSS que el teléfono no soporta se ignora y se pasa a la siguiente [F2-3gpp-ts24193-atsss A8]. En MPRTP, si desaparece una interfaz, hay que (MUST) anunciar de nuevo sin ella y liberar sus subflujos [F2-mprtp A6].
- **TCP/QUIC interior ante el desorden.**
  - Lo que llega fuera de la ventana de RACK se marca perdido de forma espuria [F9-rfc8985-rack-tlp A8].
  - El temporizador de RACK puede vencer antes de tiempo, sobre todo sin DSACK, y RACK pide marcas de envío más finas que min_RTT/4 [A9].
  - El aviso DSACK va en un solo ACK y se pierde con él [F9-rfc2883-dsack A5, A6].
  - En QUIC, un umbral de congestión persistente demasiado chico baja el throughput [F9-rfc9002-quic-loss A8].
- **Cola.** Un AQM fuera de la cola real no controla nada [F9-rfc8290-fq-codel A6, A7]. Un reparto reactivo como LetFlow no evita los desbalances cortos [F9-letflow A8].
- **Plazos.**
  - Conviene 2–3 retransmisiones como mucho, porque varias exceden la vida del dato [F4-deadline-aware-multipath A12, matizada].
  - Los errores de predicción son inevitables y cuestan redundancia [F4-joint-sched-coding-delayed-feedback A9].
  - Un respaldo que no puede llevar el bloque completo no protege, y la FEC no ayuda cuando cae la capacidad de un solo enlace [F4-joint-sched-coding-parallel, mecanismos; modelo].
- **Vendedores.** Speedify declara experimental su modo streaming, y corrigió redes celulares solo IPv6 y transportes TCP/HTTPS que no conectaban [F6-speedify-streaming-mode A5, A6]. La continuidad de su modo redundante es una promesa sin medida [F6-speedify-redundant-mode A2]. Dejero saca del pool al dispositivo prestado que se vuelve inestable, que no es lo mismo que silenciar un enlace propio [F7-dejero-us10028163 A10].

### Qué significa para cada producto
**cengarde** [razonado]:
1. Antes de la Fase 5, probar la salud (historia 006) contra un escalón de OWD y un corte de más de 1 s que se repitan cada 15 s, como hace Starlink.
2. Poner en el bonding una guarda de «no peor que el mejor enlace solo», medida con las sondas. La evidencia de campo (OMR, XLINK, dual-LTE) dice que juntar Starlink y celular puede salir peor.
3. La ventana anti-replay de WireGuard es de 8192 paquetes. Con paquetes de 1400 B equivale a ~0,9 s a 100 Mbit/s y a ~1,8 s a 50 Mbit/s. Si un enlace con segundos de cola (bufferbloat celular) entra en el reparto, el kernel descartará sus paquetes. Hoy lo evita el silenciado a 150 ms; en bonding, la cola de cada enlace tiene que quedar muy por debajo de esa cifra.
4. EAGAIN y los errores de envío nunca cuentan como caída. Cada cambio de sesión de WireGuard (rekey, índice nuevo) reinicia el contador: el estado por enlace y el reorden se indexan por (índice, contador), con test.
5. Una FEC liviana no debe reparar las pérdidas de congestión. Todo feedback nuevo (NACK, FEC, pesos) va autenticado y con ventana acotada.
6. En OpenWrt, revisar que un SQM/cake fijo sobre las VLAN 5G no pelee con el pacing. Lo sugirió el mantenedor de OMR, sin causa probada [MEDIR].

**SmoothStream** [razonado]:
1. Contar en el receptor los paquetes descartados por llegar tarde y usar ese conteo como entrada del gobernador. No bajar el buffer por debajo del presupuesto (lección de SRT).
2. Subir el peso de una pata en escalones, para no soltar una ráfaga que el módem convierta en retardo.
3. Verificar en el cable tres cosas: que rtt-drop está activo, que el perfil RIST es el esperado y que los parámetros de fec-rlc coinciden en ambas puntas. Son fallas «por defecto» del mismo tipo que las de MLVPN.
4. Para HARQ tipo II: pedido autenticado y ventana con tope, y medir el costo de CPU en el ARM cuando se pierde feedback.
5. La correlación entre operadores puede multiplicar varias veces lo que daría la independencia: 3,5× en pérdida y 4,2× en caídas, con 2 operadores en autopista. En el estadio es [MEDIR] antes de dimensionar.
6. En NSA, los handovers 5G→LTE por SCG failure y los cambios verticales son eventos de radio con caída de capacidad en ambos sentidos. La mejora de RSRP en un handover no predice el throughput, así que en T7 la entrada de radio sirve como contexto, no como predictor probado.

**Qué medir:**

- Starlink del cliente: frecuencia y duración de los cortes de más de 1 s y de las ráfagas de pérdida, y el escalón de OWD en los bordes de 15 s. Para ver lo que dura menos de un segundo hace falta una prueba con sondas más densas que cada 100 ms.
- En laboratorio (netem): respuesta de la salud de cengarde (silenciado, MUDO, espera que se duplica hasta 32 s) ante cortes de 0,5 s y de 1,5 s que se repiten cada 15 s.
- Bonding frente al mejor enlace solo, con Starlink + 5G bajo carga (análogo a OMR #3026), con y sin SQM en OpenWrt.
- 5G en CPE fijo en Chile, por operador: tasa de handovers, cambios 5G↔LTE y SCG failure (leídos del módem), y su efecto en el OWD y la pérdida.
- Correlación de pérdidas entre los cuatro operadores (en casa para cengarde, en el estadio para SmoothStream), con una definición como la de Salzburgo (no llegar dentro de X ms). Medir también la de los enlaces que comparten el adaptador USB.
- Frecuencia de EAGAIN por enlace bajo carga en la Pi, y comprobar que nunca saca un enlace.
- Rekey o cambio de sesión de WireGuard durante el bonding: pérdidas o desorden espurio en el receptor.
- Descartes por la ventana anti-replay de WireGuard cuando un enlace con cola larga lleva tráfico repartido a 50–100 Mbit/s.
- SmoothStream, en el cable: que rtt-drop esté activo, el perfil RIST efectivo, que los parámetros de fec-rlc coincidan en ambas puntas, y el conteo de descartes por tarde en el receptor.
- SmoothStream: costo de CPU en el ARM cuando se pierde el feedback de HARQ tipo II y la ventana crece hasta su tope.
- Que cada comparación de laboratorio corra con el mismo build y kernel, en hardware real, con repeticiones y dispersión.


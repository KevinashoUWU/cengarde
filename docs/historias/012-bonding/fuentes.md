# Fuentes del estudio de bonding (2026-10-10)

Cada fuente tiene un id (el que citan las matrices y los ejes entre corchetes), su tipo, el acceso que tuvo el extractor y cuántas de sus afirmaciones quedaron verificadas o matizadas por el supervisor. Las fichas completas, con las citas textuales y su ubicación, están en `fichas.json`.

## F1. Normas de broadcast con multicamino

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| F1-vsf-tr06-2-main | [VSF TR-06-2:2024 RIST Main Profile Protocol Specification](https://static.vsf.tv/download/technical_recommendations/VSF_TR-06-2_2024_06_12.pdf) | norma | completo | 8 / 2 / 0 |
| F1-vsf-tr06-3-advanced | [VSF TR-06-3:2024, RIST Advanced Profile](https://static.vsf.tv/download/technical_recommendations/VSF_TR-06-3_2024_06_12.pdf) | norma | completo | 8 / 2 / 0 |
| F1-vsf-tr06-1-simple | [VSF TR-06-1:2020 RIST Simple Profile](https://static.vsf.tv/download/technical_recommendations/VSF_TR-06-1_2020_06_25.pdf) | norma | parcial | 11 / 2 / 0 |
| F1-srt-socket-groups | [SRT Connection Bonding: Socket Groups (docs/features/socket-groups.md, Haivision/srt)](https://raw.githubusercontent.com/Haivision/srt/master/docs/features/socket-groups.md) | documentación técnica (documento de diseño del proyecto SRT) + código fuente de referencia | completo | 12 / 1 / 0 |
| F1-srt-draft | [The SRT Protocol (draft-sharabayko-srt-01)](https://datatracker.ietf.org/doc/html/draft-sharabayko-srt) | norma | parcial | 12 / 1 / 0 |

## F2. Multicamino en el IETF, 3GPP y Broadband Forum

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| F2-rfc8684-mptcp | [RFC 8684: TCP Extensions for Multipath Operation with Multiple Addresses](https://www.rfc-editor.org/rfc/rfc8684) | norma | completo | 14 / 0 / 0 |
| F2-rfc8157-gre-bonding | [RFC 8157: Huawei's GRE Tunnel Bonding Protocol](https://www.rfc-editor.org/rfc/rfc8157) | norma | completo | 10 / 2 / 0 |
| F2-bbf-tr348 | [TR-348 Hybrid Access Broadband Network Architecture (Issue 1)](https://www.broadband-forum.org/pdfs/tr-348-1-0-0.pdf) | norma | completo | 12 / 3 / 0 |
| F2-quic-multipath | [Multiple Paths for QUIC (draft-ietf-quic-multipath-21)](https://datatracker.ietf.org/doc/draft-ietf-quic-multipath/) | borrador IETF | completo | 12 / 3 / 0 |
| F2-mprtp | [Multipath RTP (MPRTP), draft-ietf-avtcore-mprtp-03](https://datatracker.ietf.org/doc/draft-ietf-avtcore-mprtp/) | Borrador IETF (avtcore), expirado y archivado; última revisión -03 (2016-07-08); nunca llegó a RFC (Intended RFC status: none) | completo | 11 / 1 / 0 |
| F2-mp-dccp | [Datagram Congestion Control Protocol (DCCP) Extensions for Multipath Operation with Multiple Addresses (MP-DCCP, RFC 9897)](https://datatracker.ietf.org/doc/draft-ietf-tsvwg-multipath-dccp/) | RFC (Proposed Standard, enero 2026); antes borrador draft-ietf-tsvwg-multipath-dccp | parcial | 10 / 1 / 0 |
| F2-3gpp-ts24193-atsss | [3GPP TS 24.193 (ATSSS), 6.1.3.1 Definición de reglas ATSSS y modos de steering](https://www.itecspec.com/3gpp/24.193/s/6.1.3.1) | norma 3GPP (espejo iTecSpec Rel-19 del 6.1.3.1), con contraste en ETSI TS 124 193 V17.9.0 | parcial | 6 / 4 / 0 |
| F2-linux-mptcp-doc | [Multipath TCP (MPTCP), documentación del kernel Linux 6.15](https://docs.kernel.org/6.15/networking/mptcp.html) | documentación técnica (kernel) | completo | 3 / 2 / 0 |

## F3. Planificadores multicamino

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| F3-ecf | [ECF: An MPTCP Path Scheduler to Manage Heterogeneous Paths](https://research.ibm.com/publications/ecf-an-mptcp-path-scheduler-to-manage-heterogeneous-paths) | paper | parcial | 3 / 2 / 0 |
| F3-blest | [BLEST: Blocking Estimation-based MPTCP Scheduler for Heterogeneous Networks](https://dl.ifip.org/db/conf/networking/networking2016/1570234725.pdf) | paper | completo | 3 / 0 / 0 |
| F3-xlink | [XLINK: QoE-Driven Multi-Path QUIC Transport in Large-scale Video Services](https://conferences.sigcomm.org/sigcomm/2021/files/papers/3452296.3472893.pdf) | paper | parcial | 8 / 2 / 0 |
| F3-qaware | [QAware: A Cross-Layer Approach to MPTCP Scheduling](https://arxiv.org/pdf/1808.04390) | paper | completo | 7 / 4 / 1 |
| F3-mptcp-sched-eval-2025 | [Evaluating the Impact of Packet Scheduling and Congestion Control Algorithms on MPTCP Performance over Heterogeneous Networks](https://arxiv.org/pdf/2511.14550) | preprint | parcial | 10 / 3 / 0 |
| F3-daps | [DAPS: Intelligent Delay-Aware Packet Scheduling for Multipath Transport](https://hal.archives-ouvertes.fr/hal-01062851) | paper | no-accesible | 0 / 0 / 0 |
| F3-raiciu-nsdi12 | [How Hard Can It Be? Designing and Implementing a Deployable Multipath TCP](https://www.usenix.org/conference/nsdi12/technical-sessions/presentation/raiciu) | paper | completo | 11 / 2 / 0 |
| F3-peekaboo | [Peekaboo: Learning-Based Multipath Scheduling for Dynamic Heterogeneous Environments](https://oda.oslomet.no/oda-xmlui/handle/10642/9989) | paper | no-accesible | 0 / 0 / 0 |
| F3-cech-thesis | [Analyzing and Realizing Multipath TCP Schedulers in Linux](https://www.nitindermohan.com/documents/student-thesis/HendrikCechGR.pdf) | tesis | completo | 9 / 3 / 0 |

## F4. Recuperación en multicamino: FEC y ARQ

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| F4-rfc8681-rlc | [RFC 8681: Sliding Window RLC FEC Schemes for FECFRAME](https://www.rfc-editor.org/rfc/rfc8681) | norma | completo | 8 / 4 / 0 |
| F4-rfc9407-tetrys | [RFC 9407: Tetrys, an On-the-Fly Network Coding Protocol](https://www.rfc-editor.org/rfc/rfc9407) | norma | completo | 11 / 3 / 0 |
| F4-quic-fec-mpquic | [Adding Forward Erasure Correction to QUIC](https://arxiv.org/abs/1809.04822) | preprint | parcial | 7 / 5 / 0 |
| F4-joint-sched-coding-parallel | [Joint Scheduling and Coding for Reliable, Latency-bounded Transmission over Parallel Wireless Links](https://arxiv.org/abs/2208.11978) | preprint | completo | 9 / 2 / 0 |
| F4-joint-sched-coding-delayed-feedback | [Joint Scheduling and Coding For Low In-Order Delivery Delay Over Lossy Paths With Delayed Feedback](https://arxiv.org/abs/1804.04921) | preprint | parcial | 7 / 2 / 0 |
| F4-deadline-aware-multipath | [Deadline-Aware Multipath Communication: An Optimization Problem](https://arxiv.org/abs/1706.05867) | preprint | parcial | 9 / 3 / 0 |
| F4-gabriel-coded-multipath | [No Plan Survives Contact with the Enemy: On Gains of Coded Multipath over MPTCP in Dynamic Settings](https://fis.tu-dresden.de/portal/en/publications/no-plan-survives-contact-with-the-enemy(9a496354-ea5e-43af-ab31-fed7f1e78ed1).html) | paper | no-accesible | 0 / 0 / 0 |
| F4-fmtcp | [FMTCP: A Fountain Code-Based Multipath Transmission Control Protocol](https://researchconnect.stonybrook.edu/en/publications/fmtcp-a-fountain-code-based-multipath-transmission-control-protoc/) | paper | parcial | 5 / 0 / 0 |

## F5. Código abierto de bonding y redundancia

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| F5-belabox-srtla | [BELABOX/srtla: SRT transport proxy with link aggregation](https://github.com/BELABOX/srtla) | código | parcial | 12 / 1 / 0 |
| F5-srtla-send-rs | [CERALIVE/srtla-send-rs: SRTLA bonding sender en Rust](https://github.com/CERALIVE/srtla-send-rs) | código | parcial | 10 / 3 / 0 |
| F5-mlvpn | [zehome/MLVPN: Multi-Link VPN](https://github.com/zehome/MLVPN) | código | parcial | 7 / 5 / 0 |
| F5-openmptcprouter | [Ysurac/openmptcprouter: agregación con MPTCP en OpenWrt](https://github.com/Ysurac/openmptcprouter) | código | parcial | 8 / 3 / 0 |
| F5-kcp-go | [xtaci/kcp-go: Reliable-UDP con FEC Reed-Solomon (Go)](https://github.com/xtaci/kcp-go) | código | parcial | 9 / 2 / 0 |

## F6. Bonding comercial de internet

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| F6-peplink-speedfusion-whitepaper | [SpeedFusion: How to Get the Most Out of SpeedFusion Technologies (ver 04.02)](https://www.venntelecom.com/documents/13/SpeedFusion_Whitepaper_ver04-02.pdf) | documentación técnica del fabricante (copia de distribuidor) | completo | 15 / 3 / 0 |
| F6-speedify-bonding-mode | [Speedify: Bonding Mode (artículo de soporte 870)](https://support.speedify.com/article/870-bonding-mode) | documentación técnica del fabricante (artículo de soporte) | completo | 6 / 0 / 0 |
| F6-speedify-redundant-mode | [Speedify Redundant Mode Overview (artículo de soporte 242)](https://support.speedify.com/article/242-redundant-mode) | documentación de soporte del fabricante (HTML, sin cifras) | completo | 5 / 2 / 0 |
| F6-speedify-streaming-mode | [Introducing Speedify 9.6 with Experimental Streaming Mode](https://speedify.com/blog/release-notes/introducing-speedify-9-6-with-experimental-streaming-mode/) | blog de release del fabricante (Connectify/Speedify) | completo | 7 / 0 / 0 |

## F7. Bonding comercial de broadcast

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| F7-liveu-lrt | [LiveU LRT (LiveU Reliable Transport)](https://www.liveu.tv/lrt) | documentación técnica del fabricante (página de producto; el brochure técnico está detrás de un formulario y no se completó) | parcial | 6 / 0 / 0 |
| F7-haivision-sst | [Haivision SST (Safe Stream Transport): página de producto](https://www.haivision.com/products/sst-protocol/) | documentación técnica del fabricante (página de producto, sin especificación) | completo | 4 / 1 / 0 |
| F7-dejero-us10028163 | [US10028163B2: System and method for transmission of data from a wireless mobile device over a multipath wireless router](https://patents.google.com/patent/US10028163B2) | patente | parcial | 12 / 3 / 0 |
| F7-meta-dams | [LiveStream Meta-DAMS: Multipath Scheduler using Hybrid Meta Reinforcement Learning for Live Video Streaming](https://ece.uvic.ca/~cai/tccn24-meta-dams.pdf) | paper (preprint, versión aceptada) | parcial | 10 / 3 / 0 |

## F8. Los enlaces: 5G, LTE y Starlink medidos

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| F8-mohan-starlink-multifaceted | [A Multifaceted Look at Starlink Performance](https://arxiv.org/pdf/2310.09242) | paper (WWW '24, Singapur) | completo | 6 / 5 / 0 |
| F8-starlink-scheduler-constellations | [Making Sense of Constellations: Methodologies for Understanding Starlink's Scheduling Algorithms](https://arxiv.org/abs/2307.00402) | preprint | parcial | 7 / 1 / 0 |
| F8-leo-reordering | [End-to-End Delivery in LEO Mega-constellations and the Reordering Problem](https://arxiv.org/html/2405.07627v1) | preprint | completo | 7 / 1 / 0 |
| F8-salzburg-multiprovider | [An Open Data Set about Multi-Provider Redundancy in Cellular Networks](https://www.salzburgresearch.at/publikation/an-open-data-set-about-multi-provider-redundancy-in-cellular-networks/) | dataset + paper (WoWMoM 2024, DOI 10.1109/WoWMoM60985.2024.00063) | completo | 6 / 2 / 0 |
| F8-dual-lte-mptcp | [Is Two Greater Than One?: Analyzing Multipath TCP over Dual-LTE in the Wild](https://arxiv.org/pdf/1909.02601) | preprint | completo | 6 / 6 / 0 |
| F8-bufferbloat-3g4g | [Tackling Bufferbloat in 3G/4G Mobile Networks](https://techrep.csc.ncsu.edu/2012/TR-2012-6.pdf) | paper | parcial | 8 / 4 / 0 |
| F8-variegated-5g | [A Variegated Look at 5G in the Wild: Performance, Power, and QoE Implications](https://doi.org/10.1145/3452296.3472923) | paper | parcial | 5 / 3 / 0 |
| F8-handover-5g-2025 | [Handover Configurations in Operational 5G Networks: Diversity, Evolution, and Impact on Performance](https://arxiv.org/pdf/2511.03116) | preprint | completo | 6 / 2 / 0 |
| F8-first-look-starlink | [A First Look at Starlink Performance](https://research.dial.uclouvain.be/handle/2078.5/255149) | paper | completo | 9 / 2 / 0 |

## F9. El tráfico interior frente al desorden y el jitter

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| F9-rfc8985-rack-tlp | [RFC 8985: The RACK-TLP Loss Detection Algorithm for TCP](https://www.rfc-editor.org/rfc/rfc8985) | norma | completo | 11 / 1 / 0 |
| F9-rfc9002-quic-loss | [RFC 9002: QUIC Loss Detection and Congestion Control](https://www.rfc-editor.org/rfc/rfc9002) | norma | completo | 10 / 1 / 0 |
| F9-linux-ip-sysctl | [Linux kernel ip-sysctl: tcp_reordering, tcp_max_reordering, tcp_dsack, tcp_recovery (RACK), tcp_early_retrans (TLP)](https://www.kernel.org/doc/html/latest/networking/ip-sysctl.html) | documentación técnica (kernel) | completo | 6 / 0 / 0 |
| F9-rfc2883-dsack | [RFC 2883: An Extension to the Selective Acknowledgement (SACK) Option for TCP](https://www.rfc-editor.org/rfc/rfc2883) | norma | completo | 9 / 0 / 0 |
| F9-wireguard-paper | [WireGuard: Next Generation Kernel Network Tunnel](https://www.wireguard.com/papers/wireguard.pdf) | paper | completo | 5 / 1 / 0 |
| F9-letflow | [Let It Flow: Resilient Asymmetric Load Balancing with Flowlet Switching (NSDI '17)](https://www.usenix.org/conference/nsdi17/technical-sessions/presentation/vanini) | paper | parcial | 9 / 0 / 1 |
| F9-bbr-draft | [draft-cardwell-iccrg-bbr-congestion-control-02 (BBRv2)](https://datatracker.ietf.org/doc/html/draft-cardwell-iccrg-bbr-congestion-control) | borrador IETF (Internet-Draft, ICCRG, versión -02; especifica BBRv2) | parcial | 8 / 0 / 0 |
| F9-rfc8290-fq-codel | [RFC 8290: The Flow Queue CoDel Packet Scheduler and Active Queue Management Algorithm](https://www.rfc-editor.org/rfc/rfc8290) | norma | completo | 9 / 0 / 0 |

## G. Huecos que pidió la auditoría

| Id | Fuente | Tipo | Acceso | Verificadas / matizadas / refutadas |
| --- | --- | --- | --- | --- |
| G1-wg-kernel | [WireGuard en el kernel Linux v6.12: ventana anti-replay y manejo de ECN (código)](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/wireguard/messages.h) | codigo | completo | 6 / 1 / 0 |
| G1-ecn-2020 | [Hilo WireGuard 2020-04: usar los helpers de ECN de RFC 6040 (Toke Høiland-Jørgensen)](https://lists.zx2c4.com/pipermail/wireguard/2020-April/005348.html) | foro | completo | 6 / 1 / 0 |
| G2-cake-autorate | [cake-autorate (lynxthecat): README, defaults.sh y CHANGELOG del repositorio](https://github.com/lynxthecat/cake-autorate) | codigo (repositorio, script bash) | completo | 11 / 1 / 0 |
| G2-foro-cake-aqm | [CAKE w/ Adaptive Bandwidth: hilo del foro de OpenWrt (forum.openwrt.org/t/191049)](https://forum.openwrt.org/t/cake-w-adaptive-bandwidth/191049) | foro (hilo de usuarios y autor de cake-autorate) | parcial | 5 / 2 / 1 |
| G3-dropbox-rack | [Boosting Dropbox upload speed and improving Windows' TCP stack](https://dropbox.tech/infrastructure/boosting-dropbox-upload-speed) | blog de ingeniería del fabricante del servicio (caso de campo, no norma ni código) | parcial | 11 / 1 / 0 |
| G3-quiche-loss | [quiche (Chromium): general_loss_algorithm.cc](https://quiche.googlesource.com/quiche/+/refs/heads/main/quiche/quic/core/congestion_control/general_loss_algorithm.cc) | código fuente (rama main, snapshot descargado el 2026-10-10) | parcial | 4 / 1 / 0 |
| G4-uplink | [Comprehensive Analysis of Cellular Uplink Performance in a Dense Stadium Deployment](https://arxiv.org/pdf/2604.04371) | paper (arXiv preprint, medición PHY con QualiPoc en estadio) | completo | 6 / 0 / 0 |
| G4-apps | [App-Based Performance Characterization of Cellular and Wi-Fi Networks in Dense Stadium Deployments](https://arxiv.org/pdf/2607.16008) | paper (arXiv preprint, QoE por app con teléfonos comerciales en estadio) | completo | 7 / 2 / 0 |
| G5-scream | [Self-Clocked Rate Adaptation for Multimedia (SCReAM)](https://www.rfc-editor.org/rfc/rfc8298.txt) | norma (RFC Experimental) | completo | 5 / 2 / 0 |
| G5-rmcat-wireless | [Evaluation Test Cases for Interactive Real-Time Media over Wireless Networks (RFC 8869)](https://www.rfc-editor.org/rfc/rfc8869.txt) | norma (RFC Informational, casos de prueba) | completo | 5 / 0 / 0 |
| G6-belabox-srtla | [BELABOX/srtla: srtla_send.c (reparto por ventana y ACK/NAK, commit 37862da, 2025-04-05)](https://github.com/BELABOX/srtla) | codigo | parcial | 5 / 2 / 0 |

## Descartadas por el curador

34 candidatas, cada una con su motivo:

- Comprimato: SRT bonding redundancy: Guía de configuración de un fabricante; fuente secundaria que no aporta algoritmo. La norma primaria está en los socket groups de SRT.
- SMPTE ST 2022-7:2019: La URL es la portada de SMPTE y la norma es de pago; la lógica de duplicación se cubre con TR-06-1/TR-06-2 y los socket groups de SRT.
- RIST (rist.tv), portal del foro: Portal sin contenido técnico propio.
- VSF Technical Recommendations (índice): Es solo un índice; se incluyen directamente los PDF de TR-06-1, TR-06-2 y TR-06-3 (los enlaces de TR-06-2 se sacaron de este índice).
- libRIST (code.videolan.org/rist/librist) y sus tags: Ya estudiado (historia 003) y el sitio está detrás de Anubis, así que no se puede leer de forma automática; sin evidencia de cambios nuevos que justifiquen re-estudiarlo.
- srtla (irlkitcom/srtla): Duplicado de BELABOX/srtla (misma obra, protocolo srtla2 experimental); no respondió desde el sandbox.
- arXiv 2508.09839: RTT de Starlink en vuelo: Escenario de aviación sobre el Pacífico, poco transferible a una antena fija en Chile; el cupo de F8 lo ocupan fuentes más directas.
- RFC 3-9 duplicados de RFC 8681 (F5, F6, F8, F9) y RFC 8684 (F4, F6, F7, F9), arXiv 2511.14550 (F1, F2, F6, F7, F8), ECF y BLEST repetidos en F2, F3, F6, F8, F9: Misma obra con otra familia; cada una queda una sola vez, en la familia donde más aporta (RFC 8681 en F4, RFC 8684 en F2, 2511.14550 y ECF/BLEST en F3).
- 3GPP TS 23.501 cláusula 5.32.8: El portal 3GPP devolvió 403 y solo hay confirmación por espejos; TS 24.193 cubre las reglas de steering.
- MPTCP project site (mptcp.dev): Portal de entrada sin contenido técnico verificable; la documentación del kernel es más precisa.
- draft-amend-tsvwg-multipath-dccp: Versión antigua de lo que hoy es draft-ietf-tsvwg-multipath-dccp, que sí se incluye.
- Low-Latency Scheduling in MPTCP (Hurtig et al., STTF): Sin versión abierta confirmada (el DOI devolvió 403 y la ficha de Simula 404); la tesis de Cech compara STTF en código.
- FALCON (arXiv 2201.08969) y survey de aprendizaje 2309.09372: Planificadores aprendidos: CPU y complejidad incompatibles con la Pi; Peekaboo y Meta-DAMS cubren ya el contraste con aprendizaje.
- More Than The Sum Of Its Parts (arXiv 1711.07565): Catalogado sin leerlo ni verificar resultados; QAware cubre la señal entre capas.
- draft-ma-quic-mpqoe, slides IETF 111 de QUIC multipath y blog de APNIC sobre XLINK: Resúmenes o diapositivas de contenido ya cubierto por el paper XLINK, que es la fuente primaria.
- RFC 8406 (taxonomía NWCRG), RFC 6363 (FECFRAME), NWCRG about: Encuadre general que no decide ningún diseño; la elección de RLC ya está tomada.
- RFC 8682 (TinyMT32): Ya implementado y verificado con vectores normativos; no re-litigar.
- RFC 6330 (RaptorQ): Descartado por el proyecto por patentes; solo documentaría una decisión ya tomada.
- Multipath TCP over network coding for mobile devices (arXiv 1306.2249): Antecedente de 2013 con aporte marginal sobre Tetrys, QUIC-FEC y la codificación conjunta ya elegidos.
- QUIC-FEC (arXiv 1904.11326): Versión posterior del mismo trabajo (1809.04822), que se incluye; se solapan.
- Kodo (Steinwurf): Librería de licencia comercial; sin aporte de diseño para este estudio.
- RFC 6356 (control de congestión acoplado): Control de congestión de subflujos TCP; cengarde no controla congestión de los flujos interiores.
- RFC 9000 (QUIC transport): El comportamiento ante desorden está en RFC 9002, que sí se incluye.
- CONGA (SIGCOMM 2014): Requiere telemetría de switches de datacenter; LetFlow cubre la idea de flowlets con menos suposiciones.
- Linux kernel: Segmentation offloads (GRO/GSO): Efecto del desorden sobre GRO es de segundo orden frente al resto; descartado por el tope de 9 en F9.
- Peplink Technology, Soporte, Foro de ingenieros: Folleto o portales sin detalle de algoritmo; el whitepaper de SpeedFusion es la fuente.
- Mushroom Networks (producto y blog), Viprinet, Bondix, Speedify Blog: Marketing sin algoritmo ni mediciones; sin patentes localizadas.
- US10833993B2 Channel bonding (Weigel Broadcasting): Solo se leyó la definición de bonding; no hay evidencia de que sus reivindicaciones aporten al reparto o a la salud de enlaces.
- engarde (porech/engarde): Línea base del fork, ya diagnosticada (historia 001).
- BELABOX (sitio del fabricante): Portal sin contenido; la documentación de bonding está en el repositorio srtla.
- TVU Router Data Sheet: Hoja de datos comercial sin algoritmo de IS+.
- LiveU Not All Bonding is the Same; Dejero Why Dejero; Google Patents por asignatario Dejero; TV Technology AVIWEST Emmy: Marketing o prensa; el índice de patentes no es un documento. La patente US10028163 se incluye directamente.
- Lumos5G dataset (IEEE DataPort): mmWave 5G con registro; los operadores chilenos no usan mmWave. Cupo de F8 ocupado por fuentes más directas.
- Breaking Through the Clouds: Starlink latency and packet loss (IFIP Networking 2025): Medición agregada de RIPE Atlas, menos útil para el gobernador que Mohan y la planificación de Starlink; sobró cupo en F8.

/* DESVÁN (no se compila): los datagramas de GRIT solo por los enlaces donde
 * llegan a tiempo (historia 031, 2026-11-03).
 *
 * Qué hacía: GRIT marcaba cada DGM (protocolo 20) en el copy[] de ops->send
 * con un bit CG_PCTL_SEND_DGM (protoctlio.h, junto a CG_PCTL_SEND_COPY), y
 * el motor de cada extremo lo mandaba solo por los enlaces donde, según su
 * ETA (eta.h), llega a menos de CG_PCTL_DGM_LATE_US (100 ms) del primero:
 *   - el reparto (client.c up_one, server.c down_prepare): elig &=
 *     cg_eta_within(&eta, elig, len, now, 100 ms), con split_eta;
 *   - las copias (client.c send_links, server.c down_send): el conjunto de
 *     cg_eta_copies_first/cg_eta_copies, o cmask, recortado a los enlaces a
 *     tiempo con c->pace (redund_pace_ms) o c->eta (split_eta).
 * Contadores: dgm_narrowed en el estado de cada extremo.
 *
 * Por qué salió (labq, remoto06 y remoto07, RTP real de 6 Mbit/s desde la
 * LAN dentro de GRIT con BOND_TSREAL=rtp BOND_TSREAL_LAN=1 BOND_UDP_GRIT=1,
 * cubic al lado, 4 × 30 s, presets de terreno; RTP perdidos por corrida):
 *   bonding    cell4 118–151 → 107–153; het 987–1265 → 946–1345: igual
 *              (el reparto por ETA ya elige el enlace que llega primero)
 *   redundancia cell4 5–7 → 50–132; het 369–657 → 444–1502: peor (el ETA es
 *              más pesimista que la realidad: las copias de los enlaces
 *              "tarde" sí llegaban dentro del jitter buffer y tapaban las
 *              pérdidas del primero)
 * Los datagramas no se botaban por viejos ni por cola llena (dgm_old y
 * dgm_full en 0): se atrasaban esperando su turno detrás del TCP de bulto
 * del mismo equipo. Lo que quedó en el motor: los datagramas sin marca van
 * en la clase de video de dscp.h (protoctl.c, flow_ds).
 *
 * Último commit con él: ninguno (se midió sin commit; el diff entero contra
 * 107c3d6 en dgm-en-plazo.diff, al lado). Para retomarlo: con un ETA más fiel (el retardo de ida
 * medido por enlace, linkowd.h) o solo para el reparto en bonding. */

/* eta.h: los enlaces de set donde un paquete de len bytes puesto ahora
 * llega a menos de window_us del primero, sin ponerlo en ninguno. */
static inline uint16_t cg_eta_within(struct cg_eta *e, uint16_t set, uint32_t len, uint64_t now_us, uint32_t window_us)
{
	uint64_t at[CG_MAX_LINKS], best = UINT64_MAX, bits = (uint64_t)len * 8 * 1000000;
	uint16_t out = 0;

	for (unsigned i = 0; i < CG_MAX_LINKS; i++) {
		struct cg_eta_link *l = &e->l[i];

		if (!(set >> i & 1))
			continue;
		if (!l->bps) {
			out |= (uint16_t)(1u << i);
			continue;
		}
		cg_eta_drain(l, now_us);
		at[i] = l->base_us + (l->backlog + bits) / l->bps;
		if (at[i] < best)
			best = at[i];
	}
	for (unsigned i = 0; best != UINT64_MAX && i < CG_MAX_LINKS; i++)
		if ((set >> i & 1) && e->l[i].bps && at[i] - best <= window_us)
			out |= (uint16_t)(1u << i);
	return out;
}

/* test (tests/test_eta.c):
 *	cg_eta_set(&e, 0, 8000000, 0, 0, 0);             1 ms
 *	cg_eta_set(&e, 1, 8000000, 50 * MS, 0, 0);       51 ms
 *	cg_eta_set(&e, 2, 8000000, 20 * MS, 300 * MS, 0); 321 ms
 *	CHECK_EQ(cg_eta_within(&e, 0x7, 1000, 0, 100 * MS), 0x3);
 *	CHECK_EQ(cg_eta_within(&e, 0x4, 1000, 0, 100 * MS), 0x4); el primero siempre
 *	CHECK_EQ(cg_eta_within(&e, 0xd, 1000, 0, 100 * MS), 0x9); sin tasa, adentro */

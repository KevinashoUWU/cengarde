# Historias

Cada historia guarda una investigación o una decisión con su evidencia, para
no tener que repetirla ni cargarla entera en el contexto. El índice activo
está en [`CLAUDE.md`](../../CLAUDE.md); aquí van la plantilla y el archivo de
historias cerradas.

## Reglas

- Una historia por tema, con nombre `NNN-tema.md` (número correlativo).
- El TL;DR tiene que bastar para decidir si hace falta leer el resto.
- Cita las fuentes con precisión (archivo:línea, versión o commit, comando
  que reproduce el dato) y separa lo verificado de lo que es estimación o
  recuerdo.
- Si los hechos cambian, se actualiza la historia: nueva fecha en la cabecera
  y una línea en "Cambios".

## Plantilla

```markdown
# NNN — Título

- **Fecha:** AAAA-MM-DD
- **Estado:** vigente | cerrada | sustituida por NNN
- **Fuentes:** enlaces, versiones o commits, archivo:línea, comandos

## TL;DR

Tres a seis viñetas con lo que hay que saber aunque no se lea el resto.

## Contexto

## Hallazgos

## Qué hacemos con esto

## Pendiente

## Cambios

- AAAA-MM-DD: creada.
```

## Archivo

Historias cerradas o sustituidas: ninguna todavía.

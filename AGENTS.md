# AGENTS.md — TreeFrogUI_picoarch (fork R36SX)

**Versión:** 1.0
**Fecha:** 2026-09-01
**Repositorio:** https://github.com/ozkaoz/TreeFrogUI_picoarch
**Upstream:** https://github.com/tzubertowski/TreeFrogUI_picoarch
**Rama base del fork:** `r36sx`

---

## 1. PROTOCOLO DE INICIO

1. **Leer:** este `AGENTS.md`.
2. **Verificar estado desde Git:**
   ```bash
   git branch --show-current && git rev-parse HEAD && git status --short --branch
   ```
3. Confirmar objetivo antes de editar.

## 2. JERARQUÍA DE FUENTES DE VERDAD

```
1. Requisito explícito actual del usuario
2. Este AGENTS.md
3. Evidencia directa (Git, builds, dispositivo)
4. Upstream tzubertowski/TreeFrogUI_picoarch (referencia)
```

## 3. ENTORNO DE COMPILACIÓN (donde buscar)

| Qué | Dónde |
|-----|-------|
| Este repo | `D:\GitHub\TreeFrogUI_picoarch` (= `/mnt/d/Github/TreeFrogUI_picoarch`) |
| Toolchain MIPS | WSL: `~/sf3000-work/sf3000toolchain/` — **no mover** |
| Builds WSL activos | `~/sf3000-work/treefrog-ui-r36sx-build/`, `~/sf3000-work/FrogUI/` |
| Repo hermano (frontend) | `D:\GitHub\treefrog-ui-r36sx` (este fork provee el picoarch que usa) |

picoarch es el **frontend libretro** (maneja display `/dev/dis`, audio ALSA, ciclo de vida de cores). Los cambios aquí afectan a TODOS los cores que corren sobre TreeFrogUI.

## 4. TRABAJO LOCAL SIN PUSH (documentado)

- Branch local `feature/fn-button-mapping` (HEAD `543699b`, +2 archivos dirty): soporte físico del botón FN. NO existe en el remoto — este repo es su única copia.
- Branch `diagnostic/audio-idle-noise-ab`: tracking a un remote WSL local (`/home/dafunknoise/sf3000-work/picoarch-audio-diag`).

Antes de cualquier `git clean`/reset en este repo: verificar esas branches.

## 5. INVARIANTES

- Los cambios de display/audio (rutas software vs hardware) requieren validación en dispositivo físico — compilar no es validar.
- Un crash de picoarch tumba TODO TreeFrogUI (es el proceso padre) — cambios al lifecycle/bifurcación (`fork+waitpid`) son clase máxima de riesgo.
- No publicar releases sin aprobación explícita del usuario.

## 6. STOP CONDITIONS

Detener ante: cambios que afecten el handshake de display, bifurcación de procesos, o input (cubevol `/tmp/joy_key`) sin plan de validación física.

## 7. HANDOFF

```
CHANGE_CLASS= FILES_CHANGED= HEAD= CHECKS_RUN= DEVICE_EVIDENCE= BLOCKER= NEXT_EXACT_ACTION= STOP_CONDITION=
```

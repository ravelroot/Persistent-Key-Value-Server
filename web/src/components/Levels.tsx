import type { Level, SSTable } from '../types'
import { bytes, dec, num } from '../format'

interface StrataProps {
  levels: Level[]
  selected: number | null
  onSelect: (t: SSTable, level: number) => void
}

export function Strata({ levels, selected, onSelect }: StrataProps) {
  const max = Math.max(1, ...levels.map((l) => l.bytes))
  return (
    <div className="strata">
      {levels.map((l) => {
        const share = l.bytes / max
        const target = l.max_bytes > 0 ? l.max_bytes : 0
        return (
          <div key={l.level} className="stratum">
            <div className="stratum-head">
              <span className="stratum-name">L{l.level}</span>
              <span className="stratum-info">
                {num(l.files)} {l.files === 1 ? 'SSTable' : 'SSTables'} · {bytes(l.bytes)}
                {target ? <> de {bytes(target)}</> : l.level === 0 ? <> · gatilho por quantidade</> : null}
              </span>
              <span className={`stratum-score${l.score >= 1 ? ' hot' : ''}`} title="Pontuação de compactação: a partir de 1 o nível é compactado">
                {dec(l.score, 2)}
              </span>
            </div>
            <div className="stratum-band" style={{ width: l.files === 0 ? '100%' : `${Math.max(8, share * 100).toFixed(1)}%` }}>
              {l.tables.length === 0 ? (
                <span className="stratum-void">{l.files > 0 ? `${num(l.files)} ${l.files === 1 ? 'arquivo' : 'arquivos'}` : 'vazio'}</span>
              ) : (
                l.tables.map((t) => (
                  <button
                    key={t.number}
                    type="button"
                    className={`sst${selected === t.number ? ' sst-on' : ''}${t.deletions > 0 && t.deletions / Math.max(1, t.entries) > 0.3 ? ' sst-tomb' : ''}`}
                    style={{ flexGrow: Math.max(1, t.bytes) }}
                    title={`#${t.number} · ${bytes(t.bytes)} · ${t.smallest} → ${t.largest}`}
                    onClick={() => onSelect(t, l.level)}
                  >
                    <span>{t.number}</span>
                  </button>
                ))
              )}
            </div>
          </div>
        )
      })}
    </div>
  )
}

interface CoreSampleProps {
  levels: Level[]
  liveFiles?: number[]
  memtableBytes: number
  memtableEntries: number
  immutableBytes: number
  compacting: boolean
}

const CORE_HEIGHT = 420
const BAND_MIN = 30

/**
 * Coluna de estratos: a memtable (memória, âmbar) por cima e os níveis L0 a L6 (disco) por baixo.
 * A espessura de cada faixa cresce com a raiz quadrada do tamanho, para os níveis pequenos continuarem legíveis.
 */
export function CoreSample({ levels, liveFiles, memtableBytes, memtableEntries, immutableBytes, compacting }: CoreSampleProps) {
  const bands = [
    { id: 'mem', name: 'Memtable', bytes: memtableBytes, files: -1, score: 0, warm: true },
    ...(immutableBytes > 0 ? [{ id: 'imm', name: 'Imutável', bytes: immutableBytes, files: -1, score: 0, warm: true }] : []),
    ...levels.map((l) => ({ id: 'l' + l.level, name: 'L' + l.level, bytes: l.bytes, files: liveFiles?.[l.level] ?? l.files, score: l.score, warm: false, level: l.level })),
  ]
  const weights = bands.map((b) => Math.sqrt(Math.max(0, b.bytes)))
  const total = weights.reduce((a, b) => a + b, 0)
  const spare = Math.max(0, CORE_HEIGHT - BAND_MIN * bands.length)
  const deepest = levels.reduce((d, l) => (l.files > 0 ? l.level : d), -1)

  return (
    <figure className={`core${compacting ? ' core-compacting' : ''}`} aria-label="Coluna de estratos da LSM-Tree">
      <div className="core-scale" aria-hidden="true">
        <span>memória</span>
        <span>disco</span>
      </div>
      <ol className="core-bands">
        {bands.map((b, i) => {
          const h = BAND_MIN + (total > 0 ? (weights[i] / total) * spare : spare / bands.length)
          const empty = b.bytes === 0 && b.files <= 0
          const lvl = 'level' in b ? (b.level as number) : -1
          const grains = b.files > 0 ? Math.min(b.files, 28) : 0
          return (
            <li
              key={b.id}
              className={`core-band${b.warm ? ' core-warm' : ` core-l${lvl}`}${empty ? ' core-empty' : ''}${b.score >= 1 ? ' core-due' : ''}`}
              style={{ flexGrow: Number(h.toFixed(1)) }}
            >
              <span className="core-rock" aria-hidden="true">
                {grains > 1 ? Array.from({ length: grains }, (_, g) => <i key={g} />) : null}
              </span>
              <span className="core-name">{b.name}</span>
              <span className="core-meta">
                {b.warm ? (
                  <>
                    <b>{bytes(b.bytes)}</b>
                    {b.id === 'mem' ? <> · {num(memtableEntries)} entradas</> : <> · aguardando gravação</>}
                  </>
                ) : empty ? (
                  lvl > deepest ? 'ainda vazio' : 'vazio'
                ) : (
                  <>
                    <b>{bytes(b.bytes)}</b> · {num(b.files)} {b.files === 1 ? 'arquivo' : 'arquivos'}
                    {b.score >= 1 ? <em title="Pontuação de compactação acima de 1">compactar</em> : null}
                  </>
                )}
              </span>
            </li>
          )
        })}
      </ol>
      <figcaption>
        Escritas entram por cima, na memória. Quando a memtable enche, vira um arquivo no L0; a compactação empurra os dados para os níveis de baixo.
        {compacting ? <strong> Compactando agora.</strong> : null}
      </figcaption>
    </figure>
  )
}

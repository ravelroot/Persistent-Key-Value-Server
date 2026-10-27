import { useState } from 'react'
import { api } from '../api'
import { Checkpoints } from '../components/Checkpoints'
import { Strata } from '../components/Levels'
import { Badge, Button, Empty, ErrorBox, KV, Loading, Panel } from '../components/ui'
import { ago, bytes, dateTime, dec, ms, num, pct } from '../format'
import { useAsync, useSession, useToast } from '../state'
import type { SSTable } from '../types'

type Action = 'flush' | 'compact'

export function Storage() {
  const { can } = useSession()
  const toast = useToast()
  const { data, error, reload } = useAsync(() => api.storage(), [], 3000)
  const [busy, setBusy] = useState<Action | null>(null)
  const [selected, setSelected] = useState<{ t: SSTable; level: number } | null>(null)

  const act = async (a: Action) => {
    setBusy(a)
    try {
      const res = a === 'flush' ? await api.flush() : await api.compact()
      toast.ok(res.message)
      reload()
    } catch (e) {
      toast.error(e)
    } finally {
      setBusy(null)
    }
  }

  if (error && !data) return <ErrorBox message={error} onRetry={reload} />
  if (!data) return <Loading />
  const e = data.engine
  const current = selected ? data.levels[selected.level]?.tables.find((t) => t.number === selected.t.number) ?? selected.t : null

  return (
    <div className="storage-layout">
      <div className="stack">
        <Panel
          title="Estratos da LSM-Tree"
          actions={
            can('operator') ? (
              <>
                <Button small icon="download" busy={busy === 'flush'} disabled={busy !== null} onClick={() => act('flush')}>
                  Gravar memtable
                </Button>
                <Button small icon="layers" busy={busy === 'compact'} disabled={busy !== null} onClick={() => act('compact')}>
                  Compactar tudo
                </Button>
              </>
            ) : undefined
          }
        >
          <div className="memtable-line">
            <span className="mem-swatch" aria-hidden="true" />
            <span>
              Memtable <b>{bytes(e.memtable_bytes)}</b> · {num(e.memtable_entries)} entradas
            </span>
            {e.immutable_bytes > 0 ? <Badge tone="warn">imutável {bytes(e.immutable_bytes)} aguardando gravação</Badge> : null}
            {e.compacting ? <Badge tone="warn">compactação em andamento</Badge> : null}
            <span className="spacer" />
            <span className="muted mono">WAL #{e.wal_number} · {bytes(e.wal_bytes)}</span>
          </div>
          <Strata levels={data.levels} selected={current?.number ?? null} onSelect={(t, level) => setSelected({ t, level })} />
          <p className="note">
            Cada bloco é uma SSTable, com largura proporcional ao tamanho. Escritas entram na memtable, viram arquivos no L0 e descem de nível nas compactações. Clique em um bloco para ver o intervalo de chaves.
          </p>
        </Panel>

        <Panel title="Compactações recentes" flush>
          {data.recent_compactions.length === 0 ? (
            <Empty title="Nenhuma compactação registrada">Use “Compactar tudo” para forçar uma, ou espere um nível passar do limite.</Empty>
          ) : (
            <div className="table-wrap">
              <table className="grid">
                <thead>
                  <tr>
                    <th>Quando</th>
                    <th>Níveis</th>
                    <th>Motivo</th>
                    <th className="num">Arquivos</th>
                    <th className="num">Lido</th>
                    <th className="num">Gravado</th>
                    <th className="num">Duração</th>
                  </tr>
                </thead>
                <tbody>
                  {data.recent_compactions.map((c, i) => (
                    <tr key={i}>
                      <td title={dateTime(c.at)}>{ago(c.at)}</td>
                      <td>
                        <span className="level-path">
                          <i className={`lv lv-${c.from_level}`} />L{c.from_level}
                          <span aria-label="para"> → </span>
                          <i className={`lv lv-${c.to_level}`} />L{c.to_level}
                        </span>
                      </td>
                      <td>{c.reason === 'size' ? 'tamanho' : c.reason}</td>
                      <td className="num">
                        {num(c.inputs)} → {num(c.outputs)}
                      </td>
                      <td className="num">{bytes(c.bytes_read)}</td>
                      <td className="num">{bytes(c.bytes_written)}</td>
                      <td className="num">{ms(c.ms)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </Panel>
      </div>

      <div className="stack">
        <Panel title={current ? `SSTable #${current.number}` : 'SSTable'}>
          {current ? (
            <KV
              items={[
                ['Nível', `L${selected?.level}`],
                ['Tamanho', bytes(current.bytes)],
                ['Entradas', num(current.entries)],
                ['Marcas de exclusão', `${num(current.deletions)} (${pct(current.entries ? current.deletions / current.entries : 0)})`],
                ['Menor chave', <span className="mono break">{current.smallest}</span>],
                ['Maior chave', <span className="mono break">{current.largest}</span>],
              ]}
            />
          ) : (
            <Empty title="Selecione um bloco">Os detalhes do arquivo aparecem aqui.</Empty>
          )}
        </Panel>
        <Checkpoints />
        <Panel title="Totais">
          <KV
            items={[
              ['SSTables', `${num(e.sst_files)} · ${bytes(e.sst_bytes)}`],
              ['Gravações da memtable', num(e.flushes)],
              ['Compactações', num(e.compactions)],
              ['Amplificação de escrita', `${dec(e.write_amplification, 2)}×`],
              ['Cache de blocos', `${bytes(e.block_cache_bytes)} de ${bytes(e.block_cache_capacity)}`],
              ['Acertos do cache', pct(e.block_cache_hits + e.block_cache_misses ? e.block_cache_hits / (e.block_cache_hits + e.block_cache_misses) : 0)],
              ['Bloom evitou leituras', `${num(e.bloom_useful)} de ${num(e.bloom_checks)}`],
              ['Escritas que esperaram', num(e.write_stalls)],
              ['Snapshots abertos', num(e.snapshots)],
            ]}
          />
        </Panel>
      </div>
    </div>
  )
}

import { api } from '../api'
import { LineChart, Meter } from '../components/Charts'
import { CoreSample } from '../components/Levels'
import { Badge, Empty, ErrorBox, KV, Loading, Panel } from '../components/ui'
import { ago, bytes, dec, ms, num, pct } from '../format'
import { useAsync, useMetrics } from '../state'

function Readout({ label, value, hint, tone }: { label: string; value: React.ReactNode; hint?: React.ReactNode; tone?: 'warn' }) {
  return (
    <div className={`readout${tone ? ' readout-' + tone : ''}`}>
      <span className="readout-label">{label}</span>
      <span className="readout-value">{value}</span>
      {hint ? <span className="readout-hint">{hint}</span> : null}
    </div>
  )
}

export function Overview() {
  const { data, error, reload } = useAsync(() => api.overview(), [], 5000)
  const { history, latest } = useMetrics()

  if (error && !data) return <ErrorBox message={error} onRetry={reload} />
  if (!data) return <Loading />

  const e = data.engine
  const ops = latest?.ops_per_sec ?? data.commands.ops_per_sec
  const hitRatio = latest ? latest.cache_hit_ratio : e.block_cache_hits + e.block_cache_misses > 0 ? e.block_cache_hits / (e.block_cache_hits + e.block_cache_misses) : 0
  const bloomRatio = e.bloom_checks > 0 ? e.bloom_useful / e.bloom_checks : 0
  const keyspaceRatio = data.commands.keyspace_hits + data.commands.keyspace_misses > 0 ? data.commands.keyspace_hits / (data.commands.keyspace_hits + data.commands.keyspace_misses) : 0
  const amp = latest?.write_amplification ?? e.write_amplification
  const reads = latest?.reads_per_sec ?? 0
  const writes = latest?.writes_per_sec ?? 0

  return (
    <div className="ov">
      <Panel title="Estratos" className="ov-core" actions={<span className="panel-note">{num(e.estimated_keys)} chaves e linhas</span>}>
        <CoreSample
          levels={data.levels}
          liveFiles={latest?.level_files}
          memtableBytes={latest?.memtable_bytes ?? e.memtable_bytes}
          memtableEntries={e.memtable_entries}
          immutableBytes={latest?.immutable_bytes ?? e.immutable_bytes}
          compacting={latest?.compacting ?? e.compacting}
        />
      </Panel>

      <Panel title="Tráfego" className="ov-traffic" actions={<span className="panel-note">últimos 2 minutos</span>}>
        <div className="traffic-head">
          <div className="traffic-now">
            <strong className="figure">{dec(ops)}</strong>
            <span>operações por segundo</span>
          </div>
          <dl className="traffic-split">
            <div>
              <dt>
                <i className="swatch swatch-a" aria-hidden="true" />
                Leituras
              </dt>
              <dd>{dec(reads)}/s</dd>
            </div>
            <div>
              <dt>
                <i className="swatch swatch-b" aria-hidden="true" />
                Escritas
              </dt>
              <dd>{dec(writes)}/s</dd>
            </div>
          </dl>
        </div>
        <LineChart
          height={210}
          series={[
            { name: 'Leituras', values: history.map((m) => m.reads_per_sec), tone: 'a' },
            { name: 'Escritas', values: history.map((m) => m.writes_per_sec), tone: 'b' },
          ]}
          format={(v) => dec(v)}
        />
        <p className="traffic-total">
          {num(data.commands.reads)} leituras e {num(data.commands.writes)} escritas desde que o servidor subiu.
        </p>
      </Panel>

      <section className="readouts ov-readouts" aria-label="Indicadores">
        <Readout label="Clientes conectados" value={num(latest?.clients ?? data.clients.connected)} hint={`${num(data.clients.total)} conexões desde a partida`} />
        <Readout label="Dados em disco" value={bytes(latest?.sst_bytes ?? e.sst_bytes)} hint={`${num(latest?.sst_files ?? e.sst_files)} SSTables em ${num(data.tables)} ${data.tables === 1 ? 'tabela' : 'tabelas'} e chaves`} />
        <Readout
          label="Amplificação de escrita"
          value={`${dec(amp, 2)}×`}
          hint={amp > 10 ? 'Alta: a compactação está regravando muito. Veja Armazenamento.' : 'bytes gravados no disco para cada byte recebido'}
          tone={amp > 10 ? 'warn' : undefined}
        />
        <Readout label="Acerto do cache de blocos" value={pct(hitRatio)} hint={<Meter value={e.block_cache_bytes} max={e.block_cache_capacity} label={`${bytes(e.block_cache_bytes)} de ${bytes(e.block_cache_capacity)} em uso`} />} />
      </section>

      <div className="ov-kvs">
      <Panel title="Memória e log">
        <KV
          items={[
            ['Entradas na memtable', num(e.memtable_entries)],
            ['Memtable imutável', bytes(e.immutable_bytes)],
            ['Log de escrita (WAL)', `#${e.wal_number} · ${bytes(e.wal_bytes)}`],
            ['Sequência atual', num(e.last_sequence)],
            ['Snapshots abertos', num(e.snapshots)],
          ]}
        />
      </Panel>

      <Panel title="Motor">
        <KV
          items={[
            ['Gravações da memtable', num(e.flushes)],
            ['Compactações', num(e.compactions)],
            ['Lido / gravado compactando', `${bytes(e.compaction_bytes_read)} / ${bytes(e.compaction_bytes_written)}`],
            ['Leituras evitadas pelo Bloom', `${pct(bloomRatio)} de ${num(e.bloom_checks)}`],
            ['Escritas que esperaram', num(e.write_stalls)],
          ]}
        />
      </Panel>

      <Panel title="Comandos">
        <KV
          items={[
            ['Total processado', num(data.commands.total)],
            ['Chaves encontradas', `${pct(keyspaceRatio)} das buscas`],
            ['Chaves expiradas', num(data.commands.expired_keys)],
            ['Rede (entrada / saída)', `${bytes(data.commands.net_input_bytes)} / ${bytes(data.commands.net_output_bytes)}`],
            ['Conexões recusadas', num(data.clients.rejected)],
          ]}
        />
      </Panel>

      </div>

      <Panel title="Compactações recentes" className="ov-wide" flush>
        {data.recent_compactions.length === 0 ? (
          <Empty title="Nenhuma compactação ainda">Elas aparecem aqui quando um nível passa do limite, ou quando você compacta pela tela Armazenamento.</Empty>
        ) : (
          <div className="table-wrap">
            <table className="grid">
              <thead>
                <tr>
                  <th>Quando</th>
                  <th>Níveis</th>
                  <th>Motivo</th>
                  <th className="num">Arquivos lidos</th>
                  <th className="num">Arquivos gerados</th>
                  <th className="num">Lido</th>
                  <th className="num">Gravado</th>
                  <th className="num">Duração</th>
                </tr>
              </thead>
              <tbody>
                {data.recent_compactions.slice(0, 8).map((c, i) => (
                  <tr key={i}>
                    <td>{ago(c.at)}</td>
                    <td>
                      <span className="level-path">
                        <i className={`lv lv-${c.from_level}`} />L{c.from_level}
                        <span aria-label="para"> → </span>
                        <i className={`lv lv-${c.to_level}`} />L{c.to_level}
                      </span>
                    </td>
                    <td>
                      <Badge tone={c.reason === 'manual' ? 'accent' : 'neutral'}>{c.reason === 'size' ? 'tamanho' : c.reason}</Badge>
                    </td>
                    <td className="num">{num(c.inputs)}</td>
                    <td className="num">{num(c.outputs)}</td>
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
  )
}

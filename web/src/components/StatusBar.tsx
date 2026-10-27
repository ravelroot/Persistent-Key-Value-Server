import { api } from '../api'
import { bytes, dec, duration, num } from '../format'
import { useAsync, useMetrics } from '../state'

const stateLabel = {
  live: 'Conectado',
  connecting: 'Conectando',
  down: 'Arca fora do ar',
  offline: 'Gateway sem resposta',
} as const

/** Barra fixa no pé da área principal: estado da conexão e os números que mudam a cada segundo. */
export function StatusBar() {
  const { state, latest } = useMetrics()
  const { data } = useAsync(() => api.overview(), [], 30000)
  const server = data?.server

  return (
    <footer className="statusbar" aria-label="Estado do servidor">
      <span className={`sb-state sb-${state}`} role="status">
        <i aria-hidden="true" />
        {stateLabel[state]}
        {latest?.compacting ? <em>compactando</em> : null}
      </span>
      {server ? (
        <span className="sb-item" title="Porta do protocolo RESP">
          porta <b>{server.port}</b>
        </span>
      ) : null}
      {server ? (
        <span className="sb-item" title="Política de sincronização do log de escrita">
          fsync <b>{server.appendfsync}</b>
        </span>
      ) : null}
      <span className="sb-item">
        <b>{latest ? dec(latest.ops_per_sec) : '—'}</b> ops/s
      </span>
      <span className="sb-item sb-warm" title="Bytes na memtable (memória, ainda não gravados como SSTable)">
        memtable <b>{latest ? bytes(latest.memtable_bytes) : '—'}</b>
      </span>
      <span className="sb-item sb-wide">
        <b>{latest ? num(latest.clients) : '—'}</b> {latest?.clients === 1 ? 'cliente' : 'clientes'}
      </span>
      <span className="spacer" />
      {server ? <span className="sb-item sb-wide">no ar há {duration(server.uptime_s)}</span> : null}
      {server ? <span className="sb-item">Arca {server.version}</span> : null}
    </footer>
  )
}

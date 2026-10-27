import { useState } from 'react'
import type { FormEvent, KeyboardEvent } from 'react'
import { api, errorMessage } from '../api'
import { Icon } from '../components/Icons'
import { Badge, Button, ConfirmButton, Drawer, Empty, ErrorBox, Field, Loading, Panel, Tabs } from '../components/ui'
import { saveText, toCSV } from '../csv'
import { ago, cell, ms, num } from '../format'
import { useAsync, useLocalList, useSession, useToast } from '../state'
import type { SQLResult, SavedQuery } from '../types'

const examples: [string, string][] = [
  ['Tabelas', 'SHOW TABLES'],
  ['Estrutura', 'DESCRIBE pedidos'],
  ['Pedidos por status', 'SELECT status, COUNT(*) AS pedidos, SUM(total) AS receita FROM pedidos GROUP BY status ORDER BY receita DESC'],
  ['Clientes por cidade', "SELECT cidade, COUNT(*) AS clientes FROM clientes WHERE ativo = TRUE GROUP BY cidade ORDER BY clientes DESC LIMIT 5"],
  ['Busca por índice', "EXPLAIN SELECT * FROM pedidos WHERE status = 'pago'"],
  ['Produtos com pouco estoque', 'SELECT sku, nome, estoque FROM produtos WHERE estoque < 20 ORDER BY estoque'],
]

function stamp(): string {
  const d = new Date()
  const p = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}`
}

export function SqlConsole() {
  const { can, user } = useSession()
  const toast = useToast()
  const [query, setQuery] = useState(examples[2][1])
  const [result, setResult] = useState<SQLResult | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [history, remember] = useLocalList('arca.sql.historico', 30)
  const [side, setSide] = useState<'salvas' | 'historico'>('salvas')
  const saved = useAsync(() => api.queries(), [])
  const [saving, setSaving] = useState<{ name: string; shared: boolean } | null>(null)
  const [saveErr, setSaveErr] = useState<string | null>(null)
  const [saveBusy, setSaveBusy] = useState(false)

  const saveQuery = async (e?: FormEvent) => {
    e?.preventDefault()
    if (!saving) return
    if (!saving.name.trim()) {
      setSaveErr('Dê um nome para encontrar a consulta depois.')
      return
    }
    setSaveBusy(true)
    try {
      await api.queryCreate({ name: saving.name.trim(), query: query.trim(), shared: saving.shared })
      toast.ok(`Consulta "${saving.name.trim()}" salva.`)
      setSaving(null)
      setSide('salvas')
      saved.reload()
    } catch (err) {
      setSaveErr(errorMessage(err))
    } finally {
      setSaveBusy(false)
    }
  }

  const deleteQuery = async (q: SavedQuery) => {
    try {
      await api.queryDelete(q.id)
      toast.ok(`Consulta "${q.name}" excluída.`)
      saved.reload()
    } catch (err) {
      toast.error(err)
    }
  }

  const exportResult = () => {
    if (!result || result.kind !== 'rows') return
    const name = `arca-consulta-${stamp()}.csv`
    saveText(toCSV(result.columns, result.rows), name)
    toast.ok(`Resultado exportado para ${name}.`)
  }

  const run = async (q = query) => {
    const text = q.trim()
    if (!text) return
    setBusy(true)
    setError(null)
    try {
      const r = await api.sql(text)
      setResult(r)
      remember(text)
    } catch (e) {
      setError(errorMessage(e))
      setResult(null)
    } finally {
      setBusy(false)
    }
  }

  const onKey = (e: KeyboardEvent<HTMLTextAreaElement>) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
      e.preventDefault()
      run()
    }
  }

  return (
    <div className="sql-layout">
      <div className="stack">
        <Panel
          title="Consulta"
          actions={
            <>
              {!can('operator') ? <Badge>somente leitura</Badge> : null}
              <Button icon="bookmark" disabled={!query.trim()} onClick={() => { setSaveErr(null); setSaving({ name: '', shared: false }) }}>
                Salvar
              </Button>
              <Button variant="primary" icon="play" busy={busy} onClick={() => run()}>
                Executar
              </Button>
            </>
          }
        >
          <textarea
            className="editor mono"
            aria-label="Consulta SQL"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            onKeyDown={onKey}
            rows={7}
            spellCheck={false}
          />
          <div className="editor-foot">
            <span>Ctrl + Enter executa</span>
            <span className="spacer" />
            {examples.map(([label, q]) => (
              <button key={label} type="button" className="chip" onClick={() => setQuery(q)}>
                {label}
              </button>
            ))}
          </div>
        </Panel>

        {error ? (
          <div className="error-box sql-error" role="alert">
            <strong>Erro</strong>
            <span>{error}</span>
          </div>
        ) : null}

        {result ? (
          result.kind === 'rows' ? (
            <Panel
              flush
              title={
                <>
                  Resultado <span className="muted">· {num(result.rows.length)} {result.rows.length === 1 ? 'linha' : 'linhas'} em {ms(result.ms)}</span>
                </>
              }
              actions={
                <>
                  {result.plan ? <span className="plan mono" title="Plano de execução">{result.plan}</span> : null}
                  {result.rows.length ? (
                    <Button small icon="download" onClick={exportResult}>
                      Exportar CSV
                    </Button>
                  ) : null}
                </>
              }
            >
              {result.rows.length === 0 ? (
                <Empty title="A consulta não retornou linhas" />
              ) : (
                <div className="table-wrap table-tall">
                  <table className="grid">
                    <thead>
                      <tr>
                        {result.columns.map((c, i) => (
                          <th key={i} className={result.types[i] === 'INT' || result.types[i] === 'REAL' ? 'num' : undefined}>
                            {c}
                            <small>{result.types[i]}</small>
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {result.rows.map((r, ri) => (
                        <tr key={ri}>
                          {r.map((v, i) => (
                            <td key={i} className={`${result.types[i] === 'INT' || result.types[i] === 'REAL' ? 'num' : ''}${v === null ? ' null' : ''}`}>
                              {cell(v)}
                            </td>
                          ))}
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}
            </Panel>
          ) : (
            <Panel>
              <div className="sql-done">
                <Badge tone="ok">{result.tag}</Badge>
                <span>
                  {num(result.affected)} {result.affected === 1 ? 'linha afetada' : 'linhas afetadas'} em {ms(result.ms)}
                </span>
              </div>
            </Panel>
          )
        ) : null}
      </div>

      <Panel className="sql-history" flush>
        <div className="side-tabs">
          <Tabs value={side} onChange={setSide} options={[['salvas', 'Salvas'], ['historico', 'Histórico']]} />
        </div>
        {side === 'salvas' ? (
          saved.error && !saved.data ? (
            <div className="side-pad">
              <ErrorBox message={saved.error} onRetry={saved.reload} />
            </div>
          ) : !saved.data ? (
            <Loading />
          ) : saved.data.length === 0 ? (
            <Empty title="Nenhuma consulta salva">Escreva uma consulta e use Salvar para guardá-la aqui. Marque como compartilhada para toda a equipe ver.</Empty>
          ) : (
            <ul className="saved-list">
              {saved.data.map((q) => (
                <li key={q.id}>
                  <button type="button" className="saved-open" onClick={() => setQuery(q.query)} title={q.query}>
                    <strong>
                      {q.name}
                      {q.shared ? (
                        <span className="saved-shared" title="Compartilhada com todos os usuários">
                          <Icon name="users" size={13} /> compartilhada
                        </span>
                      ) : null}
                    </strong>
                    <code>{q.query}</code>
                    <small>
                      {q.owner === user.email ? 'sua' : q.owner} · {ago(q.created_at)}
                    </small>
                  </button>
                  {q.owner === user.email || can('admin') ? (
                    <ConfirmButton label="Excluir" confirmLabel="Excluir" onConfirm={() => deleteQuery(q)} />
                  ) : null}
                </li>
              ))}
            </ul>
          )
        ) : history.length === 0 ? (
          <Empty title="Nada executado ainda">As consultas que você executar neste navegador aparecem aqui.</Empty>
        ) : (
          <ul className="history">
            {history.map((h) => (
              <li key={h}>
                <button type="button" className="mono" onClick={() => setQuery(h)} title={h}>
                  {h}
                </button>
              </li>
            ))}
          </ul>
        )}
      </Panel>

      <Drawer
        open={saving !== null}
        title="Salvar consulta"
        subtitle="Fica disponível em qualquer navegador em que você entrar."
        onClose={() => setSaving(null)}
        footer={
          <>
            <Button variant="quiet" onClick={() => setSaving(null)}>
              Cancelar
            </Button>
            <span className="spacer" />
            <Button variant="primary" icon="bookmark" busy={saveBusy} onClick={() => saveQuery()}>
              Salvar consulta
            </Button>
          </>
        }
      >
        {saving ? (
          <form className="form" onSubmit={saveQuery}>
            <Field label="Nome" htmlFor="sq-name" error={saveErr ?? undefined}>
              <input id="sq-name" value={saving.name} onChange={(e) => setSaving({ ...saving, name: e.target.value })} placeholder="ex.: Receita por status" maxLength={80} />
            </Field>
            <label className="check">
              <input type="checkbox" checked={saving.shared} onChange={(e) => setSaving({ ...saving, shared: e.target.checked })} />
              <span>Compartilhar com todos os usuários do painel</span>
            </label>
            <Field label="Consulta">
              <pre className="code-block">{query.trim()}</pre>
            </Field>
          </form>
        ) : null}
      </Drawer>
    </div>
  )
}

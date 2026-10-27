import { useEffect, useMemo, useState } from 'react'
import type { FormEvent } from 'react'
import { ApiError, api, errorMessage } from '../api'
import { Badge, Button, ConfirmButton, Drawer, Empty, ErrorBox, Field, Loading, Panel } from '../components/ui'
import { parseCSV, readFile } from '../csv'
import { cell, dateTime, looksLikeTimestamp, num } from '../format'
import { useAsync, useSession, useToast } from '../state'
import type { Cell, Column, RowPage, Table, TableImportResult } from '../types'

interface CsvImport {
  file: string
  text: string
  header: string[]
  rows: string[][]
  unknown: string[]
  missing: string[]
}

type Draft = Record<string, string | boolean | null>

function toDraft(t: Table, row?: Cell[], columns?: string[]): Draft {
  const d: Draft = {}
  for (const c of t.columns) {
    const idx = columns ? columns.indexOf(c.name) : -1
    const v = row && idx >= 0 ? row[idx] : null
    if (c.type === 'BOOL') d[c.name] = v === null ? (row ? null : c.default === true ? true : c.default === false ? false : null) : Boolean(v)
    else d[c.name] = v === null || v === undefined ? '' : String(v)
  }
  return d
}

function fromDraft(t: Table, d: Draft, isNew: boolean): Record<string, unknown> {
  const out: Record<string, unknown> = {}
  for (const c of t.columns) {
    const v = d[c.name]
    if (isNew && c.primary_key && c.auto_increment && (v === '' || v === null)) continue
    if (!isNew && c.primary_key) continue
    if (c.type === 'BOOL') out[c.name] = v
    else if (v === '' || v === null) out[c.name] = null
    else out[c.name] = v
  }
  return out
}

function TypeTag({ c }: { c: Column }) {
  return (
    <span className={`col-tag${c.primary_key ? ' col-pk' : ''}`}>
      <b>{c.name}</b>
      <i>{c.type}</i>
      {c.primary_key ? <em>PK{c.auto_increment ? ' auto' : ''}</em> : c.not_null ? <em>NOT NULL</em> : null}
    </span>
  )
}

export function Tables() {
  const { can } = useSession()
  const toast = useToast()
  const writable = can('operator')
  const list = useAsync(() => api.tables(), [])
  const [selected, setSelected] = useState<string | null>(null)
  const [page, setPage] = useState<RowPage | null>(null)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [limit, setLimit] = useState(50)
  const [offset, setOffset] = useState(0)
  const [order, setOrder] = useState<{ col: string; dir: 'asc' | 'desc' } | null>(null)
  const [filterInput, setFilterInput] = useState('')
  const [filter, setFilter] = useState('')
  const [draft, setDraft] = useState<{ isNew: boolean; key: Cell; values: Draft } | null>(null)
  const [fieldErr, setFieldErr] = useState<Record<string, string>>({})
  const [saving, setSaving] = useState(false)
  const [reloadTick, setReloadTick] = useState(0)
  const [exporting, setExporting] = useState<'csv' | 'json' | null>(null)
  const [importOpen, setImportOpen] = useState(false)
  const [csv, setCsv] = useState<CsvImport | null>(null)
  const [csvErr, setCsvErr] = useState<string | null>(null)
  const [csvBusy, setCsvBusy] = useState(false)
  const [csvResult, setCsvResult] = useState<TableImportResult | null>(null)

  const table = useMemo(() => list.data?.find((t) => t.name === selected) ?? null, [list.data, selected])

  useEffect(() => {
    if (!selected && list.data && list.data.length) setSelected(list.data[0].name)
  }, [list.data, selected])

  useEffect(() => {
    if (!selected) return
    let alive = true
    setLoading(true)
    api
      .rows(selected, { limit, offset, order: order?.col, dir: order?.dir, filter: filter || undefined })
      .then((p) => {
        if (alive) {
          setPage(p)
          setError(null)
        }
      })
      .catch((e) => alive && setError(errorMessage(e)))
      .finally(() => alive && setLoading(false))
    return () => {
      alive = false
    }
  }, [selected, limit, offset, order, filter, reloadTick])

  const choose = (name: string) => {
    setSelected(name)
    setOffset(0)
    setOrder(null)
    setFilter('')
    setFilterInput('')
    setPage(null)
  }

  const sortBy = (col: string) => {
    setOffset(0)
    setOrder((o) => (o?.col === col ? (o.dir === 'asc' ? { col, dir: 'desc' } : null) : { col, dir: 'asc' }))
  }

  const applyFilter = (e: FormEvent) => {
    e.preventDefault()
    setOffset(0)
    setFilter(filterInput.trim())
  }

  const refresh = () => {
    setReloadTick((n) => n + 1)
    list.reload()
  }

  const pkIndex = page ? page.columns.indexOf(page.primary_key) : -1

  const exportTable = async (format: 'csv' | 'json') => {
    if (!table) return
    setExporting(format)
    try {
      const name = await api.tableExport(table.name, format)
      toast.ok(`Tabela ${table.name} exportada para ${name}.`)
    } catch (e) {
      toast.error(e)
    } finally {
      setExporting(null)
    }
  }

  const openImport = () => {
    setCsv(null)
    setCsvErr(null)
    setCsvResult(null)
    setImportOpen(true)
  }

  const pickCsv = async (file: File | undefined) => {
    if (!file || !table) return
    setCsvErr(null)
    setCsvResult(null)
    try {
      const text = await readFile(file)
      const all = parseCSV(text)
      if (all.length < 2) throw new Error('O arquivo precisa de uma linha de cabeçalho com os nomes das colunas e ao menos uma linha de dados.')
      const header = all[0].map((h) => h.trim())
      const names = table.columns.map((c) => c.name)
      const unknown = header.filter((h) => !names.includes(h))
      const missing = table.columns.filter((c) => c.not_null && !c.auto_increment && c.default === null && !header.includes(c.name)).map((c) => c.name)
      setCsv({ file: file.name, text, header, rows: all.slice(1), unknown, missing })
    } catch (e) {
      setCsv(null)
      setCsvErr(errorMessage(e))
    }
  }

  const runImport = async () => {
    if (!table || !csv) return
    setCsvBusy(true)
    setCsvErr(null)
    try {
      const r = await api.tableImport(table.name, csv.text)
      setCsvResult(r)
      if (r.failed === 0) {
        toast.ok(`${num(r.inserted)} ${r.inserted === 1 ? 'linha importada' : 'linhas importadas'} em ${table.name}.`)
        setImportOpen(false)
      }
      refresh()
    } catch (e) {
      setCsvErr(errorMessage(e))
    } finally {
      setCsvBusy(false)
    }
  }

  const save = async () => {
    if (!table || !draft) return
    setSaving(true)
    setFieldErr({})
    try {
      const values = fromDraft(table, draft.values, draft.isNew)
      if (draft.isNew) {
        await api.rowInsert(table.name, values)
        toast.ok('Linha inserida.')
      } else {
        await api.rowUpdate(table.name, draft.key, values)
        toast.ok('Linha atualizada.')
      }
      setDraft(null)
      refresh()
    } catch (e) {
      if (e instanceof ApiError && Object.keys(e.fields).length) setFieldErr(e.fields)
      toast.error(e)
    } finally {
      setSaving(false)
    }
  }

  const remove = async () => {
    if (!table || !draft) return
    setSaving(true)
    try {
      await api.rowDelete(table.name, draft.key)
      toast.ok('Linha apagada.')
      setDraft(null)
      refresh()
    } catch (e) {
      toast.error(e)
    } finally {
      setSaving(false)
    }
  }

  if (list.error && !list.data) return <ErrorBox message={list.error} onRetry={list.reload} />
  if (!list.data) return <Loading />
  if (list.data.length === 0)
    return (
      <Panel>
        <Empty title="Nenhuma tabela ainda">
          Crie a primeira no Console SQL, por exemplo: <code>CREATE TABLE notas (id INT PRIMARY KEY AUTOINCREMENT, texto TEXT)</code>
        </Empty>
      </Panel>
    )

  const pages = page ? Math.max(1, Math.ceil(page.total / limit)) : 1
  const current = Math.floor(offset / limit) + 1

  return (
    <div className="split">
      <aside className="table-list" aria-label="Tabelas">
        {list.data.map((t) => (
          <button key={t.name} type="button" className={t.name === selected ? 'table-item active' : 'table-item'} onClick={() => choose(t.name)}>
            <span className="mono">{t.name}</span>
            <small>
              {num(t.rows)} {t.rows === 1 ? 'linha' : 'linhas'} · {t.columns.length} col.
            </small>
          </button>
        ))}
      </aside>

      <div className="stack">
        {table ? (
          <Panel
            title={<span className="mono">{table.name}</span>}
            actions={
              <>
                <Button small icon="refresh" onClick={refresh}>
                  Atualizar
                </Button>
                <span className="btn-group" role="group" aria-label="Exportar tabela">
                  <Button small icon="download" busy={exporting === 'csv'} onClick={() => exportTable('csv')}>
                    CSV
                  </Button>
                  <Button small busy={exporting === 'json'} onClick={() => exportTable('json')}>
                    JSON
                  </Button>
                </span>
                {writable ? (
                  <Button small icon="upload" onClick={openImport}>
                    Importar CSV
                  </Button>
                ) : null}
                {writable ? (
                  <Button small variant="primary" icon="plus" onClick={() => { setFieldErr({}); setDraft({ isNew: true, key: null, values: toDraft(table) }) }}>
                    Nova linha
                  </Button>
                ) : null}
              </>
            }
          >
            <div className="schema">
              {table.columns.map((c) => (
                <TypeTag key={c.name} c={c} />
              ))}
            </div>
            {table.indexes.length ? (
              <div className="schema-idx">
                Índices:{' '}
                {table.indexes.map((i) => (
                  <Badge key={i.name}>
                    {i.name} ({i.column})
                  </Badge>
                ))}
              </div>
            ) : null}
          </Panel>
        ) : null}

        <form className="toolbar" onSubmit={applyFilter}>
          <div className="search search-wide">
            <span className="search-prefix mono">WHERE</span>
            <input aria-label="Filtro" className="mono" value={filterInput} onChange={(e) => setFilterInput(e.target.value)} placeholder="cidade = 'Recife' AND ativo = TRUE" spellCheck={false} />
            <Button type="submit" icon="search">
              Filtrar
            </Button>
          </div>
          {filter ? (
            <Button variant="quiet" small onClick={() => { setFilter(''); setFilterInput(''); setOffset(0) }}>
              Limpar filtro
            </Button>
          ) : null}
        </form>

        {error ? <ErrorBox message={error} /> : null}

        <Panel flush>
          {!page && loading ? (
            <Loading />
          ) : page && page.rows.length === 0 ? (
            <Empty title={filter ? 'Nenhuma linha com esse filtro' : 'Tabela vazia'} />
          ) : page ? (
            <div className={`table-wrap${loading ? ' dim' : ''}`}>
              <table className="grid grid-click">
                <thead>
                  <tr>
                    {page.columns.map((c, i) => (
                      <th key={c} className={page.types[i] === 'INT' || page.types[i] === 'REAL' ? 'num' : undefined}>
                        <button type="button" className="th-sort" onClick={() => sortBy(c)}>
                          {c}
                          {order?.col === c ? <span aria-hidden="true">{order.dir === 'asc' ? ' ↑' : ' ↓'}</span> : null}
                        </button>
                      </th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {page.rows.map((r, ri) => (
                    <tr
                      key={pkIndex >= 0 ? String(r[pkIndex]) : ri}
                      tabIndex={0}
                      onClick={() => table && (setFieldErr({}), setDraft({ isNew: false, key: pkIndex >= 0 ? r[pkIndex] : null, values: toDraft(table, r, page.columns) }))}
                      onKeyDown={(e) => e.key === 'Enter' && table && setDraft({ isNew: false, key: pkIndex >= 0 ? r[pkIndex] : null, values: toDraft(table, r, page.columns) })}
                    >
                      {r.map((v, i) => (
                        <td key={i} className={`${page.types[i] === 'INT' || page.types[i] === 'REAL' ? 'num' : ''}${v === null ? ' null' : ''}${i === pkIndex ? ' pk' : ''}`}>
                          {page.types[i] === 'BOOL' && v !== null ? (
                            <Badge tone={v ? 'ok' : 'neutral'}>{v ? 'sim' : 'não'}</Badge>
                          ) : looksLikeTimestamp(page.columns[i], page.types[i], v) ? (
                            <span title={cell(v)}>{dateTime(v as number)}</span>
                          ) : (
                            cell(v)
                          )}
                        </td>
                      ))}
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          ) : null}
          {page ? (
            <div className="pager">
              <span>
                {num(page.total)} {page.total === 1 ? 'linha' : 'linhas'} · página {num(current)} de {num(pages)}
              </span>
              <span className="spacer" />
              <label className="pager-size">
                por página
                <select value={limit} onChange={(e) => { setLimit(Number(e.target.value)); setOffset(0) }}>
                  {[25, 50, 100, 250].map((n) => (
                    <option key={n} value={n}>
                      {n}
                    </option>
                  ))}
                </select>
              </label>
              <Button small disabled={offset === 0} onClick={() => setOffset(Math.max(0, offset - limit))}>
                Anterior
              </Button>
              <Button small disabled={current >= pages} onClick={() => setOffset(offset + limit)}>
                Próxima
              </Button>
            </div>
          ) : null}
        </Panel>
      </div>

      <Drawer
        open={importOpen && table !== null}
        title="Importar CSV"
        subtitle={table ? <span className="mono">{table.name}</span> : undefined}
        onClose={() => setImportOpen(false)}
        wide
        footer={
          <>
            <Button variant="quiet" onClick={() => setImportOpen(false)}>
              {csvResult && csvResult.failed ? 'Fechar' : 'Cancelar'}
            </Button>
            <span className="spacer" />
            <Button variant="primary" icon="upload" busy={csvBusy} disabled={!csv || csv.unknown.length > 0 || csv.missing.length > 0} onClick={runImport}>
              {csv ? `Importar ${num(csv.rows.length)} ${csv.rows.length === 1 ? 'linha' : 'linhas'}` : 'Importar'}
            </Button>
          </>
        }
      >
        {table ? (
          <div className="form">
            <Field label="Arquivo CSV" htmlFor="csv-file" hint="A primeira linha traz os nomes das colunas, separados por vírgula. Colunas fora do arquivo recebem o valor padrão.">
              <input id="csv-file" type="file" accept=".csv,text/csv" onChange={(e) => pickCsv(e.target.files?.[0])} />
            </Field>
            {csvErr ? <ErrorBox message={csvErr} /> : null}
            {csv && csv.unknown.length ? (
              <ErrorBox message={`A tabela ${table.name} não tem ${csv.unknown.length === 1 ? 'a coluna' : 'as colunas'} ${csv.unknown.join(', ')}. Renomeie no cabeçalho do arquivo ou remova.`} />
            ) : null}
            {csv && csv.missing.length ? (
              <ErrorBox message={`Falta ${csv.missing.length === 1 ? 'a coluna obrigatória' : 'as colunas obrigatórias'} ${csv.missing.join(', ')} no arquivo.`} />
            ) : null}
            {csv ? (
              <div className="import-summary">
                <strong>{csv.file}</strong>
                <span>
                  {num(csv.rows.length)} {csv.rows.length === 1 ? 'linha' : 'linhas'} · prévia das primeiras {Math.min(5, csv.rows.length)}
                </span>
                <div className="table-wrap import-table">
                  <table className="grid">
                    <thead>
                      <tr>
                        {csv.header.map((h) => (
                          <th key={h} className={csv.unknown.includes(h) ? 'col-unknown' : undefined}>
                            {h}
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {csv.rows.slice(0, 5).map((r, i) => (
                        <tr key={i}>
                          {csv.header.map((_, j) => (
                            <td key={j} className={r[j] === undefined || r[j] === '' ? 'null' : undefined}>
                              {r[j] === undefined || r[j] === '' ? 'NULL' : r[j]}
                            </td>
                          ))}
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              </div>
            ) : null}
            {csvResult && csvResult.failed ? (
              <div className="import-errors" role="alert">
                <strong>
                  {num(csvResult.inserted)} {csvResult.inserted === 1 ? 'linha entrou' : 'linhas entraram'}, {num(csvResult.failed)} {csvResult.failed === 1 ? 'falhou' : 'falharam'}.
                </strong>
                <span>Corrija as linhas abaixo no arquivo e importe só elas de novo.</span>
                <ul>
                  {csvResult.errors.map((er) => (
                    <li key={er.line}>
                      <b>Linha {er.line}</b> {er.message}
                    </li>
                  ))}
                </ul>
              </div>
            ) : null}
          </div>
        ) : null}
      </Drawer>

      <Drawer
        open={draft !== null && table !== null}
        title={draft?.isNew ? 'Nova linha' : 'Linha'}
        subtitle={table && draft && !draft.isNew ? <span className="mono">{table.name} · {page?.primary_key} = {cell(draft.key)}</span> : table?.name}
        onClose={() => setDraft(null)}
        footer={
          writable && draft ? (
            <>
              {!draft.isNew ? <ConfirmButton label="Apagar" confirmLabel="Apagar linha" onConfirm={remove} busy={saving} small={false} /> : null}
              <span className="spacer" />
              <Button variant="primary" busy={saving} onClick={save}>
                {draft.isNew ? 'Inserir' : 'Salvar'}
              </Button>
            </>
          ) : undefined
        }
      >
        {table && draft ? (
          <div className="form">
            {table.columns.map((c) => {
              const v = draft.values[c.name]
              const id = 'col-' + c.name
              const locked = !writable || (!draft.isNew && c.primary_key)
              const hint = c.primary_key && c.auto_increment && draft.isNew ? 'Gerado automaticamente se ficar vazio.' : `${c.type}${c.not_null ? ' · obrigatório' : ''}`
              return (
                <Field key={c.name} label={c.name} htmlFor={id} hint={hint} error={fieldErr[c.name]}>
                  {c.type === 'BOOL' ? (
                    <select
                      id={id}
                      disabled={locked}
                      value={v === null ? '' : v ? 'true' : 'false'}
                      onChange={(e) => setDraft({ ...draft, values: { ...draft.values, [c.name]: e.target.value === '' ? null : e.target.value === 'true' } })}
                    >
                      {!c.not_null ? <option value="">NULL</option> : null}
                      <option value="true">TRUE</option>
                      <option value="false">FALSE</option>
                    </select>
                  ) : (
                    <input
                      id={id}
                      className={c.type === 'TEXT' ? undefined : 'mono'}
                      inputMode={c.type === 'INT' ? 'numeric' : c.type === 'REAL' ? 'decimal' : undefined}
                      readOnly={locked}
                      value={String(v ?? '')}
                      placeholder={c.not_null ? '' : 'NULL'}
                      onChange={(e) => setDraft({ ...draft, values: { ...draft.values, [c.name]: e.target.value } })}
                    />
                  )}
                </Field>
              )
            })}
          </div>
        ) : null}
      </Drawer>
    </div>
  )
}

import { useCallback, useEffect, useState } from 'react'
import type { FormEvent } from 'react'
import { api, errorMessage } from '../api'
import { Badge, Button, ConfirmButton, Drawer, Empty, ErrorBox, Field, Loading, Panel } from '../components/ui'
import { readFile } from '../csv'
import { bytes, num, ttl } from '../format'
import { useSession, useToast } from '../state'
import type { KeyExportItem, KeyItem, KeyValue } from '../types'

function prettyJSON(v: string): string | null {
  const t = v.trim()
  if (!(t.startsWith('{') || t.startsWith('['))) return null
  try {
    return JSON.stringify(JSON.parse(t), null, 2)
  } catch {
    return null
  }
}

function parseKeyFile(text: string): KeyExportItem[] {
  let data: unknown
  try {
    data = JSON.parse(text)
  } catch {
    throw new Error('O arquivo não é um JSON válido. Use um arquivo gerado por Exportar.')
  }
  const list = Array.isArray(data) ? data : data && typeof data === 'object' ? (data as { keys?: unknown }).keys : undefined
  if (!Array.isArray(list)) throw new Error('Não encontrei a lista "keys" no arquivo. Use um arquivo gerado por Exportar.')
  const out: KeyExportItem[] = []
  for (const item of list) {
    if (!item || typeof item !== 'object') continue
    const it = item as { key?: unknown; value?: unknown; ttl_ms?: unknown }
    if (typeof it.key !== 'string' || !it.key) continue
    out.push({
      key: it.key,
      value: typeof it.value === 'string' ? it.value : it.value === undefined || it.value === null ? '' : JSON.stringify(it.value),
      ttl_ms: typeof it.ttl_ms === 'number' && it.ttl_ms > 0 ? it.ttl_ms : null,
    })
  }
  if (!out.length) throw new Error('O arquivo não tem nenhuma chave para importar.')
  return out
}

interface Importing {
  file: string
  keys: KeyExportItem[]
  mode: 'overwrite' | 'skip'
}

interface Editing {
  isNew: boolean
  key: string
  value: string
  ttl: string
  original?: KeyValue
}

export function Keys() {
  const { can } = useSession()
  const toast = useToast()
  const writable = can('operator')
  const [pattern, setPattern] = useState('*')
  const [query, setQuery] = useState('*')
  const [keys, setKeys] = useState<KeyItem[]>([])
  const [cursor, setCursor] = useState('0')
  const [done, setDone] = useState(false)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [editing, setEditing] = useState<Editing | null>(null)
  const [saving, setSaving] = useState(false)
  const [fieldErr, setFieldErr] = useState<Record<string, string>>({})
  const [exporting, setExporting] = useState(false)
  const [importing, setImporting] = useState<Importing | null>(null)
  const [importErr, setImportErr] = useState<string | null>(null)
  const [importBusy, setImportBusy] = useState(false)
  const [importOpen, setImportOpen] = useState(false)

  const load = useCallback(async (match: string, from: string, append: boolean) => {
    setLoading(true)
    setError(null)
    try {
      let next = from
      let collected: KeyItem[] = []
      for (let i = 0; i < 8; i++) {
        const page = await api.keys(match, next, 100)
        collected = collected.concat(page.keys)
        next = page.cursor
        if (next === '0' || collected.length >= 60) break
      }
      setKeys((k) => (append ? k.concat(collected) : collected))
      setCursor(next)
      setDone(next === '0')
    } catch (e) {
      setError(errorMessage(e))
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    load(query, '0', false)
  }, [query, load])

  const search = (e: FormEvent) => {
    e.preventDefault()
    const p = pattern.trim() || '*'
    if (p === query) load(p, '0', false)
    else setQuery(p)
  }

  const open = async (k: string) => {
    try {
      const v = await api.keyGet(k)
      setFieldErr({})
      setEditing({ isNew: false, key: v.key, value: v.value, ttl: v.ttl_ms > 0 ? String(Math.ceil(v.ttl_ms / 1000)) : '', original: v })
    } catch (e) {
      toast.error(e)
      if (e instanceof Error && e.message.includes('não encontrada')) setKeys((ks) => ks.filter((x) => x.key !== k))
    }
  }

  const save = async () => {
    if (!editing) return
    const errs: Record<string, string> = {}
    if (!editing.key.trim()) errs.key = 'Informe a chave.'
    const t = editing.ttl.trim()
    if (t && (!/^\d+$/.test(t) || Number(t) <= 0)) errs.ttl = 'Use um número inteiro de segundos.'
    setFieldErr(errs)
    if (Object.keys(errs).length) return
    setSaving(true)
    try {
      await api.keyPut(editing.key, editing.value, t ? Number(t) : undefined)
      toast.ok(editing.isNew ? 'Chave criada.' : 'Chave salva.')
      setEditing(null)
      load(query, '0', false)
    } catch (e) {
      toast.error(e)
    } finally {
      setSaving(false)
    }
  }

  const remove = async () => {
    if (!editing) return
    setSaving(true)
    try {
      await api.keyDelete(editing.key)
      toast.ok('Chave apagada.')
      setKeys((ks) => ks.filter((x) => x.key !== editing.key))
      setEditing(null)
    } catch (e) {
      toast.error(e)
    } finally {
      setSaving(false)
    }
  }

  const persist = async () => {
    if (!editing) return
    setSaving(true)
    try {
      await api.keyExpire(editing.key, null)
      toast.ok('TTL removido. A chave não expira mais.')
      setEditing({ ...editing, ttl: '', original: editing.original ? { ...editing.original, ttl_ms: -1 } : undefined })
      load(query, '0', false)
    } catch (e) {
      toast.error(e)
    } finally {
      setSaving(false)
    }
  }

  const exportKeys = async () => {
    setExporting(true)
    try {
      const name = await api.keysExport(query)
      toast.ok(`Chaves com ${query} exportadas para ${name}.`)
    } catch (e) {
      toast.error(e)
    } finally {
      setExporting(false)
    }
  }

  const pickFile = async (file: File | undefined) => {
    if (!file) return
    setImportErr(null)
    try {
      const keys = parseKeyFile(await readFile(file))
      setImporting({ file: file.name, keys, mode: importing?.mode ?? 'overwrite' })
    } catch (e) {
      setImporting(null)
      setImportErr(errorMessage(e))
    }
  }

  const runImport = async () => {
    if (!importing) return
    setImportBusy(true)
    try {
      const r = await api.keysImport(importing.keys, importing.mode)
      const parts = [`${num(r.imported)} importadas`]
      if (r.skipped) parts.push(`${num(r.skipped)} puladas porque já existiam`)
      if (r.failed) parts.push(`${num(r.failed)} com falha`)
      toast.push(r.failed ? 'error' : 'ok', 'Importação concluída: ' + parts.join(', ') + '.')
      setImporting(null)
      setImportOpen(false)
      load(query, '0', false)
    } catch (e) {
      setImportErr(errorMessage(e))
    } finally {
      setImportBusy(false)
    }
  }

  const pretty = editing ? prettyJSON(editing.value) : null

  return (
    <div className="stack">
      <form className="toolbar" onSubmit={search}>
        <div className="search">
          <input aria-label="Padrão de busca" value={pattern} onChange={(e) => setPattern(e.target.value)} placeholder="sessao:*" spellCheck={false} />
          <Button type="submit" icon="search" variant="primary">
            Buscar
          </Button>
        </div>
        <span className="toolbar-hint">
          Use <code>*</code> para qualquer sequência e <code>?</code> para um caractere.
        </span>
        <span className="spacer" />
        <Button icon="download" busy={exporting} onClick={exportKeys} title={`Baixa um JSON com as chaves que casam com ${query}`}>
          Exportar
        </Button>
        {writable ? (
          <Button icon="upload" onClick={() => { setImporting(null); setImportErr(null); setImportOpen(true) }}>
            Importar
          </Button>
        ) : null}
        {writable ? (
          <Button icon="plus" variant="primary" onClick={() => { setFieldErr({}); setEditing({ isNew: true, key: '', value: '', ttl: '' }) }}>
            Nova chave
          </Button>
        ) : null}
      </form>

      {error ? <ErrorBox message={error} onRetry={() => load(query, '0', false)} /> : null}

      <Panel flush title={<>Chaves <span className="muted">· {num(keys.length)}{done ? '' : '+'} com {query}</span></>}>
        {keys.length === 0 && loading ? (
          <Loading />
        ) : keys.length === 0 ? (
          <Empty title="Nenhuma chave com esse padrão">Tente outro padrão, como <code>cache:*</code>.</Empty>
        ) : (
          <div className="table-wrap">
            <table className="grid grid-click">
              <thead>
                <tr>
                  <th>Chave</th>
                  <th>Tipo</th>
                  <th className="num">Tamanho</th>
                  <th className="num">Expira em</th>
                </tr>
              </thead>
              <tbody>
                {keys.map((k) => (
                  <tr key={k.key} tabIndex={0} onClick={() => open(k.key)} onKeyDown={(e) => e.key === 'Enter' && open(k.key)}>
                    <td className="mono key-cell">{k.key}</td>
                    <td>
                      <Badge>{k.type}</Badge>
                    </td>
                    <td className="num">{bytes(k.size)}</td>
                    <td className="num">{k.ttl_ms >= 0 ? ttl(k.ttl_ms) : <span className="muted">—</span>}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
        {!done && keys.length > 0 ? (
          <div className="panel-more">
            <Button busy={loading} onClick={() => load(query, cursor, true)}>
              Carregar mais
            </Button>
          </div>
        ) : null}
      </Panel>

      <Drawer
        open={importOpen}
        title="Importar chaves"
        subtitle="Arquivo JSON gerado por Exportar, nesta ou em outra Arca."
        onClose={() => setImportOpen(false)}
        footer={
          <>
            <Button variant="quiet" onClick={() => setImportOpen(false)}>
              Cancelar
            </Button>
            <span className="spacer" />
            <Button variant="primary" icon="upload" busy={importBusy} disabled={!importing} onClick={runImport}>
              {importing ? `Importar ${num(importing.keys.length)} ${importing.keys.length === 1 ? 'chave' : 'chaves'}` : 'Importar'}
            </Button>
          </>
        }
      >
        <div className="form">
          <Field label="Arquivo" htmlFor="kv-import-file" hint='Formato: {"keys": [{"key", "value", "ttl_ms"}]}. Até 16 MB.'>
            <input id="kv-import-file" type="file" accept="application/json,.json" onChange={(e) => pickFile(e.target.files?.[0])} />
          </Field>
          {importErr ? <ErrorBox message={importErr} /> : null}
          {importing ? (
            <>
              <div className="import-summary">
                <strong>{importing.file}</strong>
                <span>
                  {num(importing.keys.length)} {importing.keys.length === 1 ? 'chave' : 'chaves'}, {num(importing.keys.filter((k) => k.ttl_ms).length)} com TTL
                </span>
                <ul className="import-preview mono">
                  {importing.keys.slice(0, 5).map((k) => (
                    <li key={k.key}>{k.key}</li>
                  ))}
                  {importing.keys.length > 5 ? <li className="muted">e mais {num(importing.keys.length - 5)}</li> : null}
                </ul>
              </div>
              <fieldset className="choice">
                <legend>Quando a chave já existir</legend>
                <label>
                  <input type="radio" name="kv-mode" checked={importing.mode === 'overwrite'} onChange={() => setImporting({ ...importing, mode: 'overwrite' })} />
                  <span>
                    <b>Sobrescrever</b>
                    <small>O valor do arquivo substitui o atual.</small>
                  </span>
                </label>
                <label>
                  <input type="radio" name="kv-mode" checked={importing.mode === 'skip'} onChange={() => setImporting({ ...importing, mode: 'skip' })} />
                  <span>
                    <b>Manter a atual</b>
                    <small>Só cria as chaves que ainda não existem.</small>
                  </span>
                </label>
              </fieldset>
            </>
          ) : null}
        </div>
      </Drawer>

      <Drawer
        open={editing !== null}
        title={editing?.isNew ? 'Nova chave' : 'Chave'}
        subtitle={editing && !editing.isNew ? <span className="mono">{editing.key}</span> : undefined}
        onClose={() => setEditing(null)}
        wide
        footer={
          writable && editing ? (
            <>
              {!editing.isNew ? <ConfirmButton label="Apagar" confirmLabel="Apagar chave" onConfirm={remove} busy={saving} small={false} /> : null}
              {!editing.isNew && editing.original && editing.original.ttl_ms > 0 ? (
                <Button icon="clock" onClick={persist} disabled={saving}>
                  Remover TTL
                </Button>
              ) : null}
              <span className="spacer" />
              <Button variant="primary" busy={saving} onClick={save}>
                Salvar
              </Button>
            </>
          ) : undefined
        }
      >
        {editing ? (
          <div className="form">
            {editing.isNew ? (
              <Field label="Chave" htmlFor="kv-key" error={fieldErr.key}>
                <input id="kv-key" className="mono" value={editing.key} onChange={(e) => setEditing({ ...editing, key: e.target.value })} spellCheck={false} />
              </Field>
            ) : (
              <div className="meta-line">
                <span>{bytes(editing.original?.size ?? 0)}</span>
                <span>{editing.original ? ttl(editing.original.ttl_ms) : ''}</span>
                {editing.original?.binary ? <Badge tone="warn">conteúdo binário</Badge> : null}
              </div>
            )}
            <Field label="Valor" htmlFor="kv-value" hint={pretty ? 'JSON válido.' : undefined}>
              <textarea
                id="kv-value"
                className="mono"
                rows={12}
                value={editing.value}
                readOnly={!writable}
                onChange={(e) => setEditing({ ...editing, value: e.target.value })}
                spellCheck={false}
              />
            </Field>
            {pretty && pretty !== editing.value && writable ? (
              <Button small onClick={() => setEditing({ ...editing, value: pretty })}>
                Formatar JSON
              </Button>
            ) : null}
            {writable ? (
              <Field label="TTL em segundos" htmlFor="kv-ttl" error={fieldErr.ttl} hint="Deixe vazio para a chave não expirar. Salvar sem TTL remove o prazo atual.">
                <input id="kv-ttl" inputMode="numeric" value={editing.ttl} onChange={(e) => setEditing({ ...editing, ttl: e.target.value })} placeholder="ex.: 3600" />
              </Field>
            ) : null}
          </div>
        ) : null}
      </Drawer>
    </div>
  )
}

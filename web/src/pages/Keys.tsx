import { useCallback, useEffect, useState } from 'react'
import type { FormEvent } from 'react'
import { api, errorMessage } from '../api'
import { Badge, Button, ConfirmButton, Drawer, Empty, ErrorBox, Field, Loading, Panel } from '../components/ui'
import { readFile } from '../csv'
import { bytes, num, ttl } from '../format'
import { useSession, useToast } from '../state'
import type { BulkDeleteResult, KeyExportItem, KeyItem, KeyNamespaces, KeyValue } from '../types'

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
  const [spaces, setSpaces] = useState<KeyNamespaces | null>(null)
  const [spacesErr, setSpacesErr] = useState<string | null>(null)
  const [sep, setSep] = useState(':')
  const [purge, setPurge] = useState<BulkDeleteResult | null>(null)
  const [purgeOpen, setPurgeOpen] = useState(false)
  const [purgeBusy, setPurgeBusy] = useState(false)

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

  const loadSpaces = useCallback(async (separator: string) => {
    setSpacesErr(null)
    try {
      setSpaces(await api.keyNamespaces(separator, 12))
    } catch (e) {
      setSpacesErr(errorMessage(e))
    }
  }, [])

  useEffect(() => {
    loadSpaces(sep)
  }, [sep, loadSpaces])

  const pickSpace = (prefix: string) => {
    const p = prefix ? `${prefix}${sep}*` : '*'
    setPattern(p)
    setQuery(p)
  }

  const openPurge = async () => {
    setPurge(null)
    setPurgeOpen(true)
    setPurgeBusy(true)
    try {
      setPurge(await api.keysDeleteMatch(query, true))
    } catch (e) {
      toast.error(e)
      setPurgeOpen(false)
    } finally {
      setPurgeBusy(false)
    }
  }

  const runPurge = async () => {
    setPurgeBusy(true)
    try {
      const r = await api.keysDeleteMatch(query, false)
      toast.ok(`${num(r.deleted ?? 0)} ${r.deleted === 1 ? 'chave apagada' : 'chaves apagadas'} com ${query}.`)
      setPurgeOpen(false)
      load(query, '0', false)
      loadSpaces(sep)
    } catch (e) {
      toast.error(e)
    } finally {
      setPurgeBusy(false)
    }
  }

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
        {writable && query !== '*' ? (
          <Button icon="trash" onClick={openPurge} title={`Apaga todas as chaves que casam com ${query}, depois de mostrar quantas são`}>
            Apagar padrão
          </Button>
        ) : null}
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

      <Panel
        title={<>Prefixos <span className="muted">· {spaces ? `${num(spaces.total_keys)} chaves em ${num(spaces.groups)} ${spaces.groups === 1 ? 'grupo' : 'grupos'}` : 'contando'}</span></>}
        actions={
          <>
            <label className="ns-sep">
              <span>Separador</span>
              <select aria-label="Separador de prefixo" value={sep} onChange={(e) => setSep(e.target.value)}>
                {[':', '.', '/', '_', '-'].map((o) => (
                  <option key={o} value={o}>
                    {o}
                  </option>
                ))}
              </select>
            </label>
            <Button small icon="refresh" onClick={() => loadSpaces(sep)} title="Contar de novo">
              Atualizar
            </Button>
          </>
        }
      >
        {spacesErr ? (
          <ErrorBox message={spacesErr} onRetry={() => loadSpaces(sep)} />
        ) : !spaces ? (
          <Loading />
        ) : spaces.namespaces.length === 0 ? (
          <Empty title="Nenhuma chave ainda">Crie uma chave ou importe um arquivo para ver os prefixos aqui.</Empty>
        ) : (
          <ul className="ns-list">
            {spaces.namespaces.map((n) => {
              const share = spaces.total_keys ? n.keys / spaces.total_keys : 0
              const active = query === (n.prefix ? `${n.prefix}${sep}*` : '*')
              return (
                <li key={n.prefix || '\u0000'}>
                  <button type="button" className={`ns-row${active ? ' is-active' : ''}`} onClick={() => pickSpace(n.prefix)} title={n.prefix ? `Buscar ${n.prefix}${sep}*` : 'Chaves sem o separador'}>
                    <span className="ns-name mono">{n.prefix ? `${n.prefix}${sep}` : <i>sem prefixo</i>}</span>
                    <span className="ns-bar" aria-hidden="true">
                      <span style={{ width: `${Math.max(share * 100, 1.5)}%` }} />
                    </span>
                    <span className="ns-num">{num(n.keys)}</span>
                    <span className="ns-pct muted">{(share * 100).toFixed(share < 0.1 ? 1 : 0)}%</span>
                    <span className="ns-bytes muted">{bytes(n.bytes)}</span>
                    <span className="ns-ttl muted">{n.with_ttl ? `${num(n.with_ttl)} com TTL` : 'sem TTL'}</span>
                  </button>
                </li>
              )
            })}
          </ul>
        )}
        {spaces && spaces.groups > spaces.namespaces.length ? (
          <p className="ns-more muted">Mostrando os {num(spaces.namespaces.length)} maiores de {num(spaces.groups)} grupos.</p>
        ) : null}
      </Panel>

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
        open={purgeOpen}
        title="Apagar por padrão"
        subtitle={<span className="mono">{query}</span>}
        onClose={() => setPurgeOpen(false)}
        footer={
          <>
            <Button variant="quiet" onClick={() => setPurgeOpen(false)}>
              Cancelar
            </Button>
            <span className="spacer" />
            <ConfirmButton
              label={purge?.matched ? `Apagar ${num(purge.matched)} ${purge.matched === 1 ? 'chave' : 'chaves'}` : 'Apagar'}
              confirmLabel="Confirmar remoção"
              onConfirm={runPurge}
              busy={purgeBusy}
              disabled={!purge?.matched}
              small={false}
            />
          </>
        }
      >
        {!purge ? (
          <Loading />
        ) : !purge.matched ? (
          <Empty title="Nenhuma chave com esse padrão">Nada será apagado.</Empty>
        ) : (
          <div className="form">
            <div className="import-summary">
              <strong>
                {num(purge.matched)} {purge.matched === 1 ? 'chave casa' : 'chaves casam'} com o padrão
                {purge.truncated ? ' (limite de 50 000 por vez)' : ''}
              </strong>
              <span>A remoção não pode ser desfeita. Exporte antes se quiser guardar uma cópia. As chaves do painel nunca entram na conta.</span>
              <ul className="import-preview mono">
                {(purge.sample ?? []).map((k) => (
                  <li key={k}>{k}</li>
                ))}
                {purge.matched > (purge.sample?.length ?? 0) ? <li className="muted">e mais {num(purge.matched - (purge.sample?.length ?? 0))}</li> : null}
              </ul>
            </div>
          </div>
        )}
      </Drawer>

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

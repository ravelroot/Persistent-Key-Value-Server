import { useMemo, useState } from 'react'
import { ApiError, api } from '../api'
import { Badge, Button, ConfirmButton, Drawer, Empty, ErrorBox, Field, Loading, Panel, Tabs } from '../components/ui'
import { dateTime, roleLabel } from '../format'
import { useAsync, useSession, useToast } from '../state'
import type { Role, User } from '../types'

const roles: Role[] = ['reader', 'operator', 'admin']

const actionLabel: Record<string, string> = {
  login: 'entrou',
  logout: 'saiu',
  login_falhou: 'senha errada',
  senha_alterada: 'trocou a senha',
  chave_gravada: 'gravou chave',
  chave_removida: 'apagou chave',
  chaves_removidas: 'apagou chaves por padrão',
  ttl_definido: 'definiu TTL',
  ttl_removido: 'removeu TTL',
  linha_inserida: 'inseriu linha',
  linha_alterada: 'alterou linha',
  linha_removida: 'apagou linha',
  sql: 'executou SQL',
  comando: 'comando cru',
  flush: 'gravou memtable',
  compactacao: 'compactou',
  checkpoint: 'checkpoint',
  config: 'mudou configuração',
  usuario_criado: 'criou usuário',
  usuario_alterado: 'alterou usuário',
  usuario_removido: 'removeu usuário',
}

function UsersTab() {
  const { user: me } = useSession()
  const toast = useToast()
  const { data, error, reload } = useAsync(() => api.users(), [])
  const [form, setForm] = useState<{ email: string; name: string; role: Role; password: string } | null>(null)
  const [edit, setEdit] = useState<{ user: User; name: string; role: Role; password: string } | null>(null)
  const [fieldErr, setFieldErr] = useState<Record<string, string>>({})
  const [busy, setBusy] = useState(false)

  const create = async () => {
    if (!form) return
    setBusy(true)
    setFieldErr({})
    try {
      await api.userCreate(form)
      toast.ok('Usuário criado.')
      setForm(null)
      reload()
    } catch (e) {
      if (e instanceof ApiError) setFieldErr(e.fields)
      toast.error(e)
    } finally {
      setBusy(false)
    }
  }

  const update = async () => {
    if (!edit) return
    setBusy(true)
    setFieldErr({})
    const patch: { name?: string; role?: Role; password?: string } = {}
    if (edit.name !== edit.user.name) patch.name = edit.name
    if (edit.role !== edit.user.role) patch.role = edit.role
    if (edit.password) patch.password = edit.password
    try {
      await api.userUpdate(edit.user.email, patch)
      toast.ok('Usuário atualizado.')
      setEdit(null)
      reload()
    } catch (e) {
      if (e instanceof ApiError) setFieldErr(e.fields)
      toast.error(e)
    } finally {
      setBusy(false)
    }
  }

  const unlock = async (u: User) => {
    try {
      await api.userUpdate(u.email, { unlock: true })
      toast.ok('Conta desbloqueada.')
      reload()
    } catch (e) {
      toast.error(e)
    }
  }

  const remove = async () => {
    if (!edit) return
    setBusy(true)
    try {
      await api.userDelete(edit.user.email)
      toast.ok('Usuário removido.')
      setEdit(null)
      reload()
    } catch (e) {
      toast.error(e)
    } finally {
      setBusy(false)
    }
  }

  if (error && !data) return <ErrorBox message={error} onRetry={reload} />
  if (!data) return <Loading />

  return (
    <>
      <Panel
        flush
        title={`${data.length} ${data.length === 1 ? 'usuário' : 'usuários'}`}
        actions={
          <Button small variant="primary" icon="plus" onClick={() => { setFieldErr({}); setForm({ email: '', name: '', role: 'reader', password: '' }) }}>
            Novo usuário
          </Button>
        }
      >
        <div className="table-wrap">
          <table className="grid grid-click">
            <thead>
              <tr>
                <th>Nome</th>
                <th>E-mail</th>
                <th>Papel</th>
                <th>Criado em</th>
                <th>Situação</th>
              </tr>
            </thead>
            <tbody>
              {data.map((u) => (
                <tr key={u.email} tabIndex={0} onClick={() => { setFieldErr({}); setEdit({ user: u, name: u.name, role: u.role, password: '' }) }}>
                  <td>
                    {u.name}
                    {u.email === me.email ? <span className="muted"> (você)</span> : null}
                  </td>
                  <td className="mono">{u.email}</td>
                  <td>
                    <Badge tone={u.role === 'admin' ? 'accent' : u.role === 'operator' ? 'ok' : 'neutral'}>{roleLabel[u.role]}</Badge>
                  </td>
                  <td>{dateTime(u.created_at ?? 0)}</td>
                  <td onClick={(e) => e.stopPropagation()}>
                    {u.locked ? (
                      <span className="inline">
                        <Badge tone="error">bloqueada</Badge>
                        <Button small icon="lock" onClick={() => unlock(u)}>
                          Desbloquear
                        </Button>
                      </span>
                    ) : (
                      <Badge tone="ok">ativa</Badge>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </Panel>

      <Drawer
        open={form !== null}
        title="Novo usuário"
        onClose={() => setForm(null)}
        footer={
          <>
            <span className="spacer" />
            <Button variant="primary" busy={busy} onClick={create}>
              Criar
            </Button>
          </>
        }
      >
        {form ? (
          <div className="form">
            <Field label="Nome" htmlFor="nu-name" error={fieldErr.name}>
              <input id="nu-name" value={form.name} onChange={(e) => setForm({ ...form, name: e.target.value })} />
            </Field>
            <Field label="E-mail" htmlFor="nu-email" error={fieldErr.email}>
              <input id="nu-email" type="email" value={form.email} onChange={(e) => setForm({ ...form, email: e.target.value })} />
            </Field>
            <Field label="Papel" htmlFor="nu-role" error={fieldErr.role} hint="Leitura só consulta. Operador também grava dados e dispara compactações. Administrador gerencia tudo.">
              <select id="nu-role" value={form.role} onChange={(e) => setForm({ ...form, role: e.target.value as Role })}>
                {roles.map((r) => (
                  <option key={r} value={r}>
                    {roleLabel[r]}
                  </option>
                ))}
              </select>
            </Field>
            <Field label="Senha inicial" htmlFor="nu-pass" error={fieldErr.password} hint="Mínimo de 8 caracteres, com letras e números.">
              <input id="nu-pass" type="password" autoComplete="new-password" value={form.password} onChange={(e) => setForm({ ...form, password: e.target.value })} />
            </Field>
          </div>
        ) : null}
      </Drawer>

      <Drawer
        open={edit !== null}
        title={edit?.user.name ?? ''}
        subtitle={<span className="mono">{edit?.user.email}</span>}
        onClose={() => setEdit(null)}
        footer={
          edit ? (
            <>
              {edit.user.email !== me.email ? <ConfirmButton label="Remover" confirmLabel="Remover usuário" onConfirm={remove} busy={busy} small={false} /> : null}
              <span className="spacer" />
              <Button variant="primary" busy={busy} onClick={update}>
                Salvar
              </Button>
            </>
          ) : undefined
        }
      >
        {edit ? (
          <div className="form">
            <Field label="Nome" htmlFor="eu-name" error={fieldErr.name}>
              <input id="eu-name" value={edit.name} onChange={(e) => setEdit({ ...edit, name: e.target.value })} />
            </Field>
            <Field label="Papel" htmlFor="eu-role" error={fieldErr.role}>
              <select id="eu-role" value={edit.role} onChange={(e) => setEdit({ ...edit, role: e.target.value as Role })}>
                {roles.map((r) => (
                  <option key={r} value={r}>
                    {roleLabel[r]}
                  </option>
                ))}
              </select>
            </Field>
            <Field label="Nova senha" htmlFor="eu-pass" error={fieldErr.password} hint="Deixe vazio para manter a senha atual.">
              <input id="eu-pass" type="password" autoComplete="new-password" value={edit.password} onChange={(e) => setEdit({ ...edit, password: e.target.value })} />
            </Field>
          </div>
        ) : null}
      </Drawer>
    </>
  )
}

function AuditTab() {
  const { data, error, reload } = useAsync(() => api.audit(300), [], 10000)
  const [filter, setFilter] = useState('')
  const [exporting, setExporting] = useState<'csv' | 'json' | null>(null)
  const toast = useToast()
  const exportAudit = async (format: 'csv' | 'json') => {
    setExporting(format)
    try {
      const name = await api.auditExport(format)
      toast.ok(`Auditoria exportada para ${name}.`)
    } catch (e) {
      toast.error(e)
    } finally {
      setExporting(null)
    }
  }
  const rows = useMemo(() => {
    const f = filter.trim().toLowerCase()
    if (!data) return []
    if (!f) return data
    return data.filter((a) => a.email.toLowerCase().includes(f) || a.action.includes(f) || a.detail.toLowerCase().includes(f) || (actionLabel[a.action] ?? '').includes(f))
  }, [data, filter])

  if (error && !data) return <ErrorBox message={error} onRetry={reload} />
  if (!data) return <Loading />
  return (
    <>
      <div className="toolbar">
        <div className="search">
          <input aria-label="Filtrar auditoria" placeholder="Filtrar por e-mail, ação ou detalhe" value={filter} onChange={(e) => setFilter(e.target.value)} />
        </div>
        <span className="spacer" />
        <Button icon="download" busy={exporting === 'csv'} disabled={exporting !== null} onClick={() => exportAudit('csv')} title="Baixa os últimos 1 000 registros em CSV">
          Exportar CSV
        </Button>
        <Button icon="download" busy={exporting === 'json'} disabled={exporting !== null} onClick={() => exportAudit('json')} title="Baixa os últimos 1 000 registros em JSON">
          JSON
        </Button>
      </div>
      <Panel flush>
        {rows.length === 0 ? (
          <Empty title="Nenhum registro" />
        ) : (
          <div className="table-wrap table-tall">
            <table className="grid">
              <thead>
                <tr>
                  <th>Quando</th>
                  <th>Quem</th>
                  <th>Ação</th>
                  <th>Detalhe</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((a) => (
                  <tr key={a.id}>
                    <td className="nowrap">{dateTime(a.at)}</td>
                    <td className="mono">{a.email || '—'}</td>
                    <td>
                      <Badge tone={a.action === 'login_falhou' ? 'error' : a.action.startsWith('login') || a.action === 'logout' ? 'neutral' : 'accent'}>
                        {actionLabel[a.action] ?? a.action}
                      </Badge>
                    </td>
                    <td className="mono detail-cell" title={a.detail}>
                      {a.detail}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Panel>
    </>
  )
}

export function Users() {
  const [tab, setTab] = useState<'users' | 'audit'>('users')
  return (
    <div className="stack">
      <Tabs
        value={tab}
        onChange={setTab}
        options={[
          ['users', 'Usuários'],
          ['audit', 'Auditoria'],
        ]}
      />
      {tab === 'users' ? <UsersTab /> : <AuditTab />}
    </div>
  )
}

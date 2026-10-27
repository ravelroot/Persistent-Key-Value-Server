import { useCallback, useEffect, useMemo, useState } from 'react'
import { api, getToken, onUnauthorized, setToken } from './api'
import { CommandPalette } from './components/CommandPalette'
import type { PaletteItem } from './components/CommandPalette'
import { Icon } from './components/Icons'
import type { IconName } from './components/Icons'
import { Logo } from './components/Logo'
import { Shortcuts } from './components/Shortcuts'
import { StatusBar } from './components/StatusBar'
import { IconButton, Loading } from './components/ui'
import { roleLabel } from './format'
import { Account } from './pages/Account'
import { Clients } from './pages/Clients'
import { Commands } from './pages/Commands'
import { Config } from './pages/Config'
import { Integrations } from './pages/Integrations'
import { Keys } from './pages/Keys'
import { Login } from './pages/Login'
import { Overview } from './pages/Overview'
import { SqlConsole } from './pages/SqlConsole'
import { Storage } from './pages/Storage'
import { Tables } from './pages/Tables'
import { Users } from './pages/Users'
import { MetricsProvider, SessionContext, canRole, useTheme, useToast } from './state'
import type { Theme } from './state'
import type { Role, User } from './types'

export type PageId = 'visao-geral' | 'armazenamento' | 'clientes' | 'chaves' | 'tabelas' | 'sql' | 'integracoes' | 'comandos' | 'configuracao' | 'usuarios' | 'conta'

type Group = 'Operação' | 'Dados' | 'Sistema'

interface PageDef {
  id: PageId
  label: string
  title: string
  description: string
  icon: IconName
  role: Role
  group: Group | null
  /** Segunda tecla do atalho "g + tecla". */
  key: string
  keywords?: string
}

export const pages: PageDef[] = [
  { id: 'visao-geral', label: 'Visão geral', title: 'Visão geral', description: 'Como os dados descem da memória para o disco, e o tráfego agora.', icon: 'overview', role: 'reader', group: 'Operação', key: 'v', keywords: 'painel resumo ops estratos' },
  { id: 'armazenamento', label: 'Armazenamento', title: 'Armazenamento', description: 'SSTables por nível, compactações, checkpoints e verificação de integridade.', icon: 'layers', role: 'reader', group: 'Operação', key: 'a', keywords: 'sstable nível compactação checkpoint backup verificar' },
  { id: 'clientes', label: 'Clientes', title: 'Clientes e comandos lentos', description: 'Quem está conectado agora e o que demorou para responder.', icon: 'clients', role: 'reader', group: 'Operação', key: 'l', keywords: 'conexões slowlog' },
  { id: 'chaves', label: 'Chaves', title: 'Chaves', description: 'Valores do espaço chave-valor, com TTL, importação e exportação.', icon: 'keys', role: 'reader', group: 'Dados', key: 'c', keywords: 'redis valor ttl importar exportar' },
  { id: 'tabelas', label: 'Tabelas', title: 'Tabelas', description: 'Linhas, colunas e índices das tabelas SQL.', icon: 'tables', role: 'reader', group: 'Dados', key: 't', keywords: 'linhas colunas csv' },
  { id: 'sql', label: 'Console SQL', title: 'Console SQL', description: 'Consultas no dialeto SQL da Arca, com histórico e consultas salvas.', icon: 'sql', role: 'reader', group: 'Dados', key: 's', keywords: 'select query consulta' },
  { id: 'integracoes', label: 'Integrações', title: 'Integrações', description: 'Conecte aplicações, gere chaves de acesso, receba webhooks e exporte métricas.', icon: 'plug', role: 'reader', group: 'Sistema', key: 'i', keywords: 'api token webhook prometheus métricas python node go java' },
  { id: 'comandos', label: 'Comandos', title: 'Comandos', description: 'Console direto do protocolo RESP.', icon: 'terminal', role: 'admin', group: 'Sistema', key: 'm', keywords: 'resp terminal redis-cli' },
  { id: 'configuracao', label: 'Configuração', title: 'Configuração', description: 'Parâmetros do servidor em tempo de execução.', icon: 'settings', role: 'admin', group: 'Sistema', key: 'f', keywords: 'config fsync' },
  { id: 'usuarios', label: 'Usuários', title: 'Usuários e auditoria', description: 'Contas do painel e tudo o que foi feito nelas.', icon: 'users', role: 'admin', group: 'Sistema', key: 'u', keywords: 'contas papéis auditoria' },
  { id: 'conta', label: 'Minha conta', title: 'Minha conta', description: 'Seus dados, permissões e senha.', icon: 'account', role: 'reader', group: null, key: 'p', keywords: 'senha perfil' },
]

const groups: Group[] = ['Operação', 'Dados', 'Sistema']

function pageFromHash(): PageId {
  const h = window.location.hash.replace(/^#\/?/, '').split('?')[0]
  return (pages.find((p) => p.id === h)?.id ?? 'visao-geral') as PageId
}

function typing(target: EventTarget | null): boolean {
  const el = target as HTMLElement | null
  if (!el) return false
  const tag = el.tagName
  return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || el.isContentEditable
}

function Shell({ user, theme, toggleTheme, onLogout, onUser }: { user: User; theme: Theme; toggleTheme: () => void; onLogout: () => void; onUser: (u: User) => void }) {
  const [page, setPage] = useState<PageId>(pageFromHash)
  const [menu, setMenu] = useState(false)
  const [palette, setPalette] = useState(false)
  const [help, setHelp] = useState(false)
  const toast = useToast()

  useEffect(() => {
    const onHash = () => setPage(pageFromHash())
    window.addEventListener('hashchange', onHash)
    return () => window.removeEventListener('hashchange', onHash)
  }, [])

  const allowed = useMemo(() => pages.filter((p) => canRole(user, p.role)), [user])
  const def = allowed.find((p) => p.id === page) ?? allowed[0]

  useEffect(() => {
    document.title = `${def.title} · Arca`
  }, [def])

  const go = useCallback((id: PageId) => {
    window.location.hash = '/' + id
    setMenu(false)
  }, [])

  useEffect(() => {
    let pendingG = 0
    const onKey = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
        e.preventDefault()
        setPalette((v) => !v)
        return
      }
      if (e.ctrlKey || e.metaKey || e.altKey || typing(e.target) || document.querySelector('[aria-modal="true"]')) return
      if (e.key === '?') {
        e.preventDefault()
        setHelp(true)
        return
      }
      const now = Date.now()
      if (pendingG && now - pendingG < 1200) {
        pendingG = 0
        const target = allowed.find((p) => p.key === e.key.toLowerCase())
        if (target) {
          e.preventDefault()
          go(target.id)
        }
        return
      }
      if (e.key === 'g') pendingG = now
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [allowed, go])

  const session = useMemo(
    () => ({
      user,
      can: (r: Role) => canRole(user, r),
      logout: onLogout,
      refreshUser: onUser,
    }),
    [user, onLogout, onUser],
  )

  const run = useCallback(
    (label: string, fn: () => Promise<{ message: string }>) => {
      fn().then((r) => toast.ok(r?.message || label), toast.error)
    },
    [toast],
  )

  const paletteItems = useMemo<PaletteItem[]>(() => {
    const nav: PaletteItem[] = allowed.map((p) => ({
      id: 'ir:' + p.id,
      group: 'Ir para',
      label: p.label,
      hint: p.description,
      icon: p.icon,
      keywords: p.keywords,
      shortcut: ['g', p.key],
      run: () => go(p.id),
    }))
    const actions: PaletteItem[] = [
      { id: 'tema', group: 'Ações', label: theme === 'dark' ? 'Usar tema claro' : 'Usar tema escuro', icon: theme === 'dark' ? 'sun' : 'moon', keywords: 'tema escuro claro', run: toggleTheme },
      { id: 'atalhos', group: 'Ações', label: 'Ver atalhos de teclado', icon: 'command', shortcut: ['?'], keywords: 'teclado ajuda', run: () => setHelp(true) },
    ]
    if (canRole(user, 'operator')) {
      actions.push(
        { id: 'flush', group: 'Motor', label: 'Gravar a memtable em disco', hint: 'Transforma a memtable atual numa SSTable no L0.', icon: 'download', keywords: 'flush memtable', run: () => run('Memtable gravada.', api.flush) },
        { id: 'compact', group: 'Motor', label: 'Compactar todos os níveis', hint: 'Mistura as SSTables e descarta versões que ninguém mais vê.', icon: 'layers', keywords: 'compact compactação', run: () => run('Compactação iniciada.', api.compact) },
        { id: 'checkpoint', group: 'Motor', label: 'Criar checkpoint', hint: 'Cópia consistente do banco na pasta checkpoints.', icon: 'bookmark', keywords: 'backup checkpoint cópia', run: () => run('Checkpoint criado.', api.checkpoint) },
      )
    }
    actions.push({ id: 'sair', group: 'Conta', label: 'Sair', icon: 'logout', keywords: 'logout sair', run: onLogout })
    return [...nav, ...actions]
  }, [allowed, go, onLogout, run, theme, toggleTheme, user])

  return (
    <SessionContext.Provider value={session}>
      <div className={`shell${menu ? ' menu-open' : ''}`}>
        <aside className="side" aria-label="Menu">
          <div className="brand">
            <span className="brand-mark">
              <Logo size={20} />
            </span>
            <strong>Arca</strong>
          </div>
          <button type="button" className="jump" onClick={() => setPalette(true)}>
            <Icon name="search" size={16} />
            <span>Ir para…</span>
            <kbd>Ctrl K</kbd>
          </button>
          <nav aria-label="Navegação principal">
            {groups.map((g) => {
              const items = allowed.filter((p) => p.group === g)
              if (!items.length) return null
              return (
                <div key={g} className="nav-group">
                  <span className="nav-title">{g}</span>
                  {items.map((p) => (
                    <a
                      key={p.id}
                      href={`#/${p.id}`}
                      className={p.id === def.id ? 'nav-item active' : 'nav-item'}
                      aria-current={p.id === def.id ? 'page' : undefined}
                      aria-keyshortcuts={`g ${p.key}`}
                      onClick={(e) => {
                        e.preventDefault()
                        go(p.id)
                      }}
                    >
                      <Icon name={p.icon} />
                      <span>{p.label}</span>
                      <kbd className="nav-key" aria-hidden="true">
                        g {p.key}
                      </kbd>
                    </a>
                  ))}
                </div>
              )
            })}
          </nav>
          <div className="side-foot">
            <a
              href="#/conta"
              className={`who${def.id === 'conta' ? ' active' : ''}`}
              onClick={(e) => {
                e.preventDefault()
                go('conta')
              }}
            >
              <span className="avatar" aria-hidden="true">
                {user.name.trim().charAt(0).toUpperCase() || '?'}
              </span>
              <div>
                <strong>{user.name}</strong>
                <span>{roleLabel[user.role]}</span>
              </div>
            </a>
            <IconButton icon="logout" label="Sair" onClick={onLogout} />
          </div>
        </aside>
        <div className="side-scrim" onClick={() => setMenu(false)} />
        <div className="main">
          <header className="top">
            <button type="button" className="icon-btn menu-btn" aria-label="Abrir menu" onClick={() => setMenu(true)}>
              <Icon name="menu" />
            </button>
            <div className="page-head">
              <h1 className="page-title">{def.title}</h1>
              <p className="page-desc">{def.description}</p>
            </div>
            <div className="top-actions">
              <button type="button" className="icon-btn jump-mini" aria-label="Ir para (Ctrl K)" onClick={() => setPalette(true)}>
                <Icon name="search" size={17} />
              </button>
              <IconButton icon="command" label="Atalhos de teclado (?)" onClick={() => setHelp(true)} />
              <IconButton icon={theme === 'dark' ? 'sun' : 'moon'} label={theme === 'dark' ? 'Tema claro' : 'Tema escuro'} onClick={toggleTheme} />
            </div>
          </header>
          <main className="content" key={def.id}>
            {def.id === 'visao-geral' ? <Overview /> : null}
            {def.id === 'armazenamento' ? <Storage /> : null}
            {def.id === 'clientes' ? <Clients /> : null}
            {def.id === 'chaves' ? <Keys /> : null}
            {def.id === 'tabelas' ? <Tables /> : null}
            {def.id === 'sql' ? <SqlConsole /> : null}
            {def.id === 'integracoes' ? <Integrations /> : null}
            {def.id === 'comandos' ? <Commands /> : null}
            {def.id === 'configuracao' ? <Config /> : null}
            {def.id === 'usuarios' ? <Users /> : null}
            {def.id === 'conta' ? <Account theme={theme} toggleTheme={toggleTheme} /> : null}
          </main>
          <StatusBar />
        </div>
      </div>
      <CommandPalette open={palette} items={paletteItems} onClose={() => setPalette(false)} />
      <Shortcuts open={help} onClose={() => setHelp(false)} items={allowed.map((p) => ({ keys: ['g', p.key], label: p.label }))} />
    </SessionContext.Provider>
  )
}

export function App() {
  const [theme, toggleTheme] = useTheme()
  const [user, setUser] = useState<User | null>(null)
  const [checking, setChecking] = useState(() => getToken() !== null)

  const clear = useCallback(() => {
    setToken(null)
    setUser(null)
  }, [])

  useEffect(() => {
    onUnauthorized(clear)
    if (!getToken()) return
    api
      .me()
      .then(setUser)
      .catch(() => clear())
      .finally(() => setChecking(false))
  }, [clear])

  const logout = useCallback(() => {
    api.logout().catch(() => undefined)
    clear()
  }, [clear])

  if (checking) {
    return (
      <div className="boot">
        <span className="brand-mark">
          <Logo size={24} />
        </span>
        <Loading label="Abrindo o painel…" />
      </div>
    )
  }

  if (!user) return <Login onLogin={setUser} />

  return (
    <MetricsProvider onExpired={clear}>
      <Shell user={user} theme={theme} toggleTheme={toggleTheme} onLogout={logout} onUser={setUser} />
    </MetricsProvider>
  )
}

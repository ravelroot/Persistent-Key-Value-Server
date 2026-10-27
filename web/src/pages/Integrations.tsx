import { useEffect, useMemo, useState } from 'react'
import type { FormEvent } from 'react'
import { ApiError, api, errorMessage } from '../api'
import { Icon } from '../components/Icons'
import { Badge, Button, ConfirmButton, Drawer, Empty, ErrorBox, Field, KV, Loading, Panel, Tabs, copyText } from '../components/ui'
import { ago, dateTime, ms, num, roleLabel } from '../format'
import { useAsync, useSession, useToast } from '../state'
import type { ApiToken, Integrations as IntegrationInfo, Role, Webhook, WebhookDelivery, WebhookEvent } from '../types'

type TabId = 'conectar' | 'chaves' | 'webhooks' | 'prometheus'

const TAB_KEY = 'arca.integracoes.aba'
const LANG_KEY = 'arca.integracoes.linguagem'

function readPref(key: string): string | null {
  try {
    return window.localStorage.getItem(key)
  } catch {
    return null
  }
}

function writePref(key: string, value: string): void {
  try {
    window.localStorage.setItem(key, value)
  } catch {
    /* navegador sem armazenamento: só não lembra a escolha */
  }
}

/** Bloco de código com botão de copiar. */
function Snippet({ code, label }: { code: string; label: string }) {
  const toast = useToast()
  const [copied, setCopied] = useState(false)
  useEffect(() => {
    if (!copied) return
    const t = window.setTimeout(() => setCopied(false), 1800)
    return () => window.clearTimeout(t)
  }, [copied])
  const copy = async () => {
    if (await copyText(code)) setCopied(true)
    else toast.push('error', 'O navegador bloqueou a área de transferência. Selecione o texto e copie com Ctrl + C.')
  }
  return (
    <div className="snippet">
      <div className="snippet-head">
        <span>{label}</span>
        <Button small variant="quiet" icon={copied ? 'check' : 'copy'} onClick={copy} aria-label={`Copiar ${label}`}>
          {copied ? 'Copiado' : 'Copiar'}
        </Button>
      </div>
      <pre className="snippet-code">
        <code>{code}</code>
      </pre>
    </div>
  )
}

/** Segredo mostrado uma única vez, com aviso e botão de copiar. */
function SecretReveal({ value, what }: { value: string; what: string }) {
  return (
    <div className="secret-reveal" role="status">
      <div className="secret-warn">
        <Icon name="lock" size={16} />
        <span>
          Copie {what} agora. A Arca guarda só um resumo dela e não consegue mostrá-la de novo.
        </span>
      </div>
      <Snippet code={value} label={what.replace(/^(a|o) /, '')} />
    </div>
  )
}

/* ------------------------------------------------------------------ Conectar */

type Lang = 'cli' | 'python' | 'node' | 'go' | 'java' | 'curl'

const langs: [Lang, string][] = [
  ['cli', 'redis-cli'],
  ['python', 'Python'],
  ['node', 'Node.js'],
  ['go', 'Go'],
  ['java', 'Java'],
  ['curl', 'API REST'],
]

function snippets(info: IntegrationInfo): Record<Lang, { label: string; code: string; note: string }> {
  const host = info.redis_host || 'localhost'
  const port = info.redis_port || 7379
  const base = info.http_base || window.location.origin
  const pass = info.requires_password
  return {
    cli: {
      label: 'Terminal',
      note: 'Qualquer redis-cli 6 ou mais novo funciona. A senha fica no .env do servidor.',
      code: [
        `export ARCA_PASSWORD='sua-senha'`,
        `redis-cli -h ${host} -p ${port}${pass ? ' -a "$ARCA_PASSWORD"' : ''} PING`,
        `redis-cli -h ${host} -p ${port}${pass ? ' -a "$ARCA_PASSWORD"' : ''} SET pedido:1001 '{"status":"pago"}' EX 3600`,
        `redis-cli -h ${host} -p ${port}${pass ? ' -a "$ARCA_PASSWORD"' : ''} SQL.JSON "SELECT sku, nome FROM produtos LIMIT 5"`,
      ].join('\n'),
    },
    python: {
      label: 'Python · redis-py',
      note: 'pip install redis. Os comandos de SQL vão por execute_command.',
      code: `import json
import os

import redis

arca = redis.Redis(
    host="${host}",
    port=${port},${pass ? '\n    password=os.environ["ARCA_PASSWORD"],' : ''}
    decode_responses=True,
)

arca.set("pedido:1001", json.dumps({"status": "pago"}), ex=3600)
print(arca.get("pedido:1001"))

produtos = arca.execute_command("SQL.JSON", "SELECT sku, nome FROM produtos LIMIT 5")
print(json.loads(produtos))
`,
    },
    node: {
      label: 'Node.js · ioredis',
      note: 'npm install ioredis. Use call() para os comandos próprios da Arca.',
      code: `import Redis from 'ioredis'

const arca = new Redis({
  host: '${host}',
  port: ${port},${pass ? '\n  password: process.env.ARCA_PASSWORD,' : ''}
})

await arca.set('pedido:1001', JSON.stringify({ status: 'pago' }), 'EX', 3600)
console.log(await arca.get('pedido:1001'))

const produtos = await arca.call('SQL.JSON', 'SELECT sku, nome FROM produtos LIMIT 5')
console.log(JSON.parse(produtos))
`,
    },
    go: {
      label: 'Go · go-redis v9',
      note: 'go get github.com/redis/go-redis/v9. Protocol: 2 porque a Arca fala RESP2.',
      code: `package main

import (
	"context"
	"fmt"
	"os"
	"time"

	"github.com/redis/go-redis/v9"
)

func main() {
	ctx := context.Background()
	arca := redis.NewClient(&redis.Options{
		Addr:     "${host}:${port}",${pass ? '\n\t\tPassword: os.Getenv("ARCA_PASSWORD"),' : ''}
		Protocol: 2,
	})

	arca.Set(ctx, "pedido:1001", \`{"status":"pago"}\`, time.Hour)
	fmt.Println(arca.Get(ctx, "pedido:1001").Val())

	produtos, err := arca.Do(ctx, "SQL.JSON", "SELECT sku, nome FROM produtos LIMIT 5").Text()
	if err != nil {
		panic(err)
	}
	fmt.Println(produtos)
}
`,
    },
    java: {
      label: 'Java · Jedis 5',
      note: 'Maven: redis.clients:jedis. sendCommand aceita os comandos próprios da Arca.',
      code: `import redis.clients.jedis.Jedis;
import redis.clients.jedis.params.SetParams;

public class ExemploArca {
    public static void main(String[] args) {
        try (Jedis arca = new Jedis("${host}", ${port})) {${pass ? '\n            arca.auth(System.getenv("ARCA_PASSWORD"));' : ''}
            arca.set("pedido:1001", "{\\"status\\":\\"pago\\"}", SetParams.setParams().ex(3600));
            System.out.println(arca.get("pedido:1001"));

            Object produtos = arca.sendCommand(() -> "SQL.JSON".getBytes(), "SELECT sku, nome FROM produtos LIMIT 5");
            System.out.println(new String((byte[]) produtos));
        }
    }
}
`,
    },
    curl: {
      label: 'HTTP · curl',
      note: 'Crie uma chave de acesso na aba ao lado. O esquema completo está no OpenAPI.',
      code: `TOKEN='arca_...'   # chave de acesso criada em Integrações

curl -s "${base}/api/keys/value?key=pedido:1001" \\
  -H "Authorization: Bearer $TOKEN"

curl -s -X POST "${base}/api/sql" \\
  -H "Authorization: Bearer $TOKEN" \\
  -H 'Content-Type: application/json' \\
  -d '{"query":"SELECT sku, nome FROM produtos LIMIT 5"}'

curl -s "${base}/api/keys/export?match=pedido:*" \\
  -H "Authorization: Bearer $TOKEN" -o pedidos.json
`,
    },
  }
}

function Connect({ info }: { info: IntegrationInfo }) {
  const [lang, setLang] = useState<Lang>(() => {
    const saved = readPref(LANG_KEY)
    return langs.some(([l]) => l === saved) ? (saved as Lang) : 'cli'
  })
  const choose = (l: Lang) => {
    setLang(l)
    writePref(LANG_KEY, l)
  }
  const all = useMemo(() => snippets(info), [info])
  const current = all[lang]
  const base = info.http_base || window.location.origin
  return (
    <div className="integ-connect">
      <Panel title="Endereços">
        <KV
          items={[
            ['Servidor (RESP2)', <span className="mono" key="r">{info.redis_host}:{info.redis_port}</span>],
            ['Senha', info.requires_password ? 'exigida pelo AUTH (ARCA_PASSWORD no .env)' : 'não configurada'],
            ['API REST', <span className="mono" key="a">{base}/api</span>],
            [
              'Esquema OpenAPI',
              <a key="o" href={info.openapi_url} target="_blank" rel="noreferrer" className="link-ext">
                <span className="mono">{info.openapi_url}</span>
                <Icon name="external" size={13} />
              </a>,
            ],
            ['Métricas Prometheus', <span className="mono" key="m">{base}{info.metrics_url}</span>],
            ['Versão', info.version],
          ]}
        />
      </Panel>
      <Panel title="Primeiro código">
        <div className="lang-switch" role="radiogroup" aria-label="Linguagem do exemplo">
          {langs.map(([l, label]) => (
            <button key={l} type="button" role="radio" aria-checked={l === lang} className={l === lang ? 'lang active' : 'lang'} onClick={() => choose(l)}>
              {label}
            </button>
          ))}
        </div>
        <p className="integ-note">{current.note}</p>
        <Snippet code={current.code} label={current.label} />
      </Panel>
    </div>
  )
}

/* ---------------------------------------------------------- Chaves de acesso */

const roleOrder: Role[] = ['reader', 'operator', 'admin']

const expiryOptions: [string, string][] = [
  ['30', '30 dias'],
  ['90', '90 dias'],
  ['365', '1 ano'],
  ['', 'Não expira'],
]

function TokensTab() {
  const { user, can } = useSession()
  const toast = useToast()
  const [all, setAll] = useState(false)
  const list = useAsync(() => api.tokens(all), [all])
  const [form, setForm] = useState<{ name: string; role: Role; expires: string } | null>(null)
  const [fieldErr, setFieldErr] = useState<Record<string, string>>({})
  const [busy, setBusy] = useState(false)
  const [created, setCreated] = useState<{ token: string; name: string } | null>(null)

  const allowedRoles = roleOrder.filter((r) => roleOrder.indexOf(r) <= roleOrder.indexOf(user.role))

  const open = () => {
    setFieldErr({})
    setCreated(null)
    setForm({ name: '', role: 'reader', expires: '90' })
  }

  const submit = async (e?: FormEvent) => {
    e?.preventDefault()
    if (!form) return
    const errs: Record<string, string> = {}
    if (!form.name.trim()) errs.name = 'Dê um nome que diga onde a chave vai ser usada, como "Grafana" ou "ETL noturno".'
    setFieldErr(errs)
    if (Object.keys(errs).length) return
    setBusy(true)
    try {
      const r = await api.tokenCreate({ name: form.name.trim(), role: form.role, expires_days: form.expires ? Number(form.expires) : null })
      setCreated({ token: r.token, name: r.info.name })
      setForm(null)
      list.reload()
    } catch (err) {
      if (err instanceof ApiError && Object.keys(err.fields).length) setFieldErr(err.fields)
      else toast.error(err)
    } finally {
      setBusy(false)
    }
  }

  const revoke = async (t: ApiToken) => {
    try {
      await api.tokenDelete(t.id)
      toast.ok(`Chave "${t.name}" revogada. Quem a usava perde o acesso agora.`)
      list.reload()
    } catch (err) {
      toast.error(err)
    }
  }

  const items = list.data ?? []

  return (
    <div className="stack">
      <div className="toolbar">
        <p className="integ-lead">
          Chaves de acesso deixam scripts, ETLs e o Prometheus chamarem a API sem usar a sua senha. Cada uma tem um papel próprio e pode ser revogada a qualquer momento.
        </p>
        <span className="spacer" />
        {can('admin') ? (
          <label className="check">
            <input type="checkbox" checked={all} onChange={(e) => setAll(e.target.checked)} />
            <span>Mostrar de todos os usuários</span>
          </label>
        ) : null}
        <Button variant="primary" icon="plus" onClick={open}>
          Criar chave de acesso
        </Button>
      </div>

      {created ? (
        <Panel title={`Chave "${created.name}" criada`} actions={<Button small variant="quiet" onClick={() => setCreated(null)}>Já copiei</Button>}>
          <SecretReveal value={created.token} what="a chave" />
          <p className="integ-note">
            Use no cabeçalho <code>Authorization: Bearer {created.token.slice(0, 10)}…</code> em qualquer rota da API e em <code>/metrics</code>.
          </p>
        </Panel>
      ) : null}

      <Panel flush title={<>Chaves de acesso <span className="muted">· {num(items.length)}</span></>}>
        {list.error && !list.data ? (
          <div className="side-pad">
            <ErrorBox message={list.error} onRetry={list.reload} />
          </div>
        ) : !list.data ? (
          <Loading />
        ) : items.length === 0 ? (
          <Empty title="Nenhuma chave de acesso">Crie uma para cada sistema que vai falar com a Arca. Assim dá para revogar um sem afetar os outros.</Empty>
        ) : (
          <div className="table-wrap">
            <table className="grid">
              <thead>
                <tr>
                  <th>Nome</th>
                  <th>Papel</th>
                  <th>Início da chave</th>
                  {all ? <th>Dono</th> : null}
                  <th>Último uso</th>
                  <th>Expira</th>
                  <th aria-label="Ações" />
                </tr>
              </thead>
              <tbody>
                {items.map((t) => {
                  const expired = t.expires_at !== null && t.expires_at < Date.now()
                  return (
                    <tr key={t.id}>
                      <td>
                        <strong className="token-name">{t.name}</strong>
                        <small className="muted"> criada {ago(t.created_at)}</small>
                      </td>
                      <td>
                        <Badge tone={t.role === 'admin' ? 'warn' : t.role === 'operator' ? 'accent' : 'neutral'}>{roleLabel[t.role] ?? t.role}</Badge>
                      </td>
                      <td className="mono">{t.prefix}…</td>
                      {all ? <td>{t.owner}</td> : null}
                      <td title={t.last_used_at ? dateTime(t.last_used_at) : undefined}>{t.last_used_at ? ago(t.last_used_at) : <span className="muted">nunca</span>}</td>
                      <td>
                        {t.expires_at === null ? (
                          <span className="muted">não expira</span>
                        ) : expired ? (
                          <Badge tone="error">expirou</Badge>
                        ) : (
                          <span title={dateTime(t.expires_at)}>{dateTime(t.expires_at).split(' ')[0]}</span>
                        )}
                      </td>
                      <td className="num">
                        <ConfirmButton label="Revogar" confirmLabel="Revogar chave" onConfirm={() => revoke(t)} />
                      </td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          </div>
        )}
      </Panel>

      <Drawer
        open={form !== null}
        title="Criar chave de acesso"
        subtitle="A chave aparece uma única vez depois de criada."
        onClose={() => setForm(null)}
        footer={
          <>
            <Button variant="quiet" onClick={() => setForm(null)}>
              Cancelar
            </Button>
            <span className="spacer" />
            <Button variant="primary" icon="token" busy={busy} onClick={() => submit()}>
              Criar chave de acesso
            </Button>
          </>
        }
      >
        {form ? (
          <form className="form" onSubmit={submit}>
            <Field label="Nome" htmlFor="tk-name" error={fieldErr.name} hint="Onde a chave vai ser usada.">
              <input id="tk-name" value={form.name} onChange={(e) => setForm({ ...form, name: e.target.value })} placeholder="ex.: Grafana" maxLength={60} />
            </Field>
            <Field label="Papel" htmlFor="tk-role" error={fieldErr.role} hint="Nunca acima do seu. Para o Prometheus, Leitura basta.">
              <select id="tk-role" value={form.role} onChange={(e) => setForm({ ...form, role: e.target.value as Role })}>
                {allowedRoles.map((r) => (
                  <option key={r} value={r}>
                    {roleLabel[r]}
                  </option>
                ))}
              </select>
            </Field>
            <Field label="Validade" htmlFor="tk-exp" error={fieldErr.expires_days}>
              <select id="tk-exp" value={form.expires} onChange={(e) => setForm({ ...form, expires: e.target.value })}>
                {expiryOptions.map(([v, label]) => (
                  <option key={v} value={v}>
                    {label}
                  </option>
                ))}
              </select>
            </Field>
          </form>
        ) : null}
      </Drawer>
    </div>
  )
}

/* ----------------------------------------------------------------- Webhooks */

interface HookForm {
  id: string | null
  url: string
  events: string[]
  active: boolean
}

function statusTone(status: number | null): 'ok' | 'error' | 'neutral' {
  if (status === null) return 'neutral'
  return status >= 200 && status < 300 ? 'ok' : 'error'
}

const verifyCode = `import hashlib
import hmac

def assinatura_valida(corpo: bytes, cabecalho: str, segredo: str) -> bool:
    esperado = "sha256=" + hmac.new(segredo.encode(), corpo, hashlib.sha256).hexdigest()
    return hmac.compare_digest(esperado, cabecalho)

# cabecalho = request.headers["X-Arca-Signature"]
`

function WebhooksTab() {
  const toast = useToast()
  const list = useAsync(() => api.webhooks(), [])
  const events = useAsync(() => api.webhookEvents(), [])
  const [form, setForm] = useState<HookForm | null>(null)
  const [fieldErr, setFieldErr] = useState<Record<string, string>>({})
  const [busy, setBusy] = useState(false)
  const [secret, setSecret] = useState<{ url: string; value: string } | null>(null)
  const [deliveries, setDeliveries] = useState<WebhookDelivery[] | null>(null)
  const [testing, setTesting] = useState(false)

  const eventLabel = useMemo(() => {
    const m = new Map<string, string>()
    for (const e of events.data ?? []) m.set(e.id, e.label)
    return m
  }, [events.data])

  const loadDeliveries = async (id: string) => {
    setDeliveries(null)
    try {
      setDeliveries(await api.webhookDeliveries(id))
    } catch {
      setDeliveries([])
    }
  }

  const openNew = () => {
    setFieldErr({})
    setDeliveries(null)
    setForm({ id: null, url: 'https://', events: ['key.write', 'table.write'], active: true })
  }

  const openEdit = (w: Webhook) => {
    setFieldErr({})
    setForm({ id: w.id, url: w.url, events: [...w.events], active: w.active })
    loadDeliveries(w.id)
  }

  const toggleEvent = (id: string) => {
    if (!form) return
    setForm({ ...form, events: form.events.includes(id) ? form.events.filter((e) => e !== id) : [...form.events, id] })
  }

  const submit = async (e?: FormEvent) => {
    e?.preventDefault()
    if (!form) return
    const errs: Record<string, string> = {}
    if (!/^https?:\/\/[^\s/]+/.test(form.url.trim())) errs.url = 'Use um endereço completo, começando com https:// ou http://.'
    if (!form.events.length) errs.events = 'Escolha ao menos um evento.'
    setFieldErr(errs)
    if (Object.keys(errs).length) return
    setBusy(true)
    try {
      if (form.id) {
        await api.webhookUpdate(form.id, { url: form.url.trim(), events: form.events, active: form.active })
        toast.ok('Webhook salvo.')
      } else {
        const r = await api.webhookCreate({ url: form.url.trim(), events: form.events, active: form.active })
        if (r.secret) setSecret({ url: r.webhook.url, value: r.secret })
        toast.ok('Webhook criado.')
      }
      setForm(null)
      list.reload()
    } catch (err) {
      if (err instanceof ApiError && Object.keys(err.fields).length) setFieldErr(err.fields)
      else toast.error(err)
    } finally {
      setBusy(false)
    }
  }

  const setActive = async (w: Webhook, active: boolean) => {
    try {
      await api.webhookUpdate(w.id, { active })
      toast.ok(active ? 'Webhook retomado.' : 'Webhook pausado. Nenhum evento será enviado até retomar.')
      list.reload()
    } catch (err) {
      toast.error(err)
    }
  }

  const test = async () => {
    if (!form?.id) return
    setTesting(true)
    try {
      const r = await api.webhookTest(form.id)
      if (r.error || r.status < 200 || r.status >= 300) toast.push('error', `O destino respondeu ${r.status || 'sem status'}${r.error ? ': ' + r.error : ''}. Confira o endereço e se ele aceita POST.`)
      else toast.ok(`Teste entregue: ${r.status} em ${ms(r.ms)}.`)
      loadDeliveries(form.id)
      list.reload()
    } catch (err) {
      toast.error(err)
    } finally {
      setTesting(false)
    }
  }

  const rotate = async () => {
    if (!form?.id) return
    try {
      const r = await api.webhookUpdate(form.id, { rotate_secret: true })
      if (r.secret) setSecret({ url: r.webhook.url, value: r.secret })
      toast.ok('Segredo novo gerado. O anterior deixou de valer.')
      setForm(null)
    } catch (err) {
      toast.error(err)
    }
  }

  const remove = async () => {
    if (!form?.id) return
    try {
      await api.webhookDelete(form.id)
      toast.ok('Webhook excluído.')
      setForm(null)
      list.reload()
    } catch (err) {
      toast.error(err)
    }
  }

  const items = list.data ?? []
  const evs: WebhookEvent[] = events.data ?? []

  return (
    <div className="integ-hooks">
      <div className="stack">
        <div className="toolbar">
          <p className="integ-lead">Avise outros sistemas quando algo muda na Arca. Cada evento vira um POST em JSON, assinado com HMAC-SHA256.</p>
          <span className="spacer" />
          <Button variant="primary" icon="plus" onClick={openNew}>
            Criar webhook
          </Button>
        </div>

        {secret ? (
          <Panel title="Segredo do webhook" actions={<Button small variant="quiet" onClick={() => setSecret(null)}>Já copiei</Button>}>
            <p className="integ-note">
              Destino <span className="mono">{secret.url}</span>
            </p>
            <SecretReveal value={secret.value} what="o segredo" />
          </Panel>
        ) : null}

        <Panel flush title={<>Webhooks <span className="muted">· {num(items.length)}</span></>}>
          {list.error && !list.data ? (
            <div className="side-pad">
              <ErrorBox message={list.error} onRetry={list.reload} />
            </div>
          ) : !list.data ? (
            <Loading />
          ) : items.length === 0 ? (
            <Empty title="Nenhum webhook">Crie um para avisar um Slack, um n8n ou a sua própria API sempre que chaves, tabelas ou o armazenamento mudarem.</Empty>
          ) : (
            <ul className="hook-list">
              {items.map((w) => (
                <li key={w.id} className={w.active ? undefined : 'hook-paused'}>
                  <button type="button" className="hook-main" onClick={() => openEdit(w)}>
                    <span className="hook-url mono">{w.url}</span>
                    <span className="hook-events">
                      {w.events.map((e) => (
                        <span key={e} className="hook-event" title={eventLabel.get(e) ?? e}>
                          {e}
                        </span>
                      ))}
                    </span>
                  </button>
                  <span className="hook-state">
                    {w.active ? <Badge tone="ok">ativo</Badge> : <Badge>pausado</Badge>}
                    {w.last_status !== null ? (
                      <span className="hook-last" title={w.last_delivery_at ? dateTime(w.last_delivery_at) : undefined}>
                        <Badge tone={statusTone(w.last_status)}>{w.last_status || 'falhou'}</Badge>
                        {w.last_delivery_at ? ago(w.last_delivery_at) : ''}
                      </span>
                    ) : (
                      <span className="muted">sem entregas</span>
                    )}
                    {w.failures > 0 ? <span className="hook-fail">{num(w.failures)} {w.failures === 1 ? 'falha seguida' : 'falhas seguidas'}</span> : null}
                  </span>
                  <Button small variant="quiet" onClick={() => setActive(w, !w.active)}>
                    {w.active ? 'Pausar' : 'Retomar'}
                  </Button>
                </li>
              ))}
            </ul>
          )}
        </Panel>
      </div>

      <Panel title="Como validar a assinatura" className="integ-aside">
        <p className="integ-note">
          Cada entrega traz <code>X-Arca-Event</code>, <code>X-Arca-Delivery</code> e <code>X-Arca-Signature</code>. Recalcule o HMAC do corpo bruto com o segredo e compare antes de confiar no conteúdo.
        </p>
        <Snippet code={verifyCode} label="Python" />
        <Snippet code={`{\n  "id": "d_01J…",\n  "event": "key.write",\n  "at": 1793110156760,\n  "actor": "operador@arca.dev",\n  "detail": "pedido:1001"\n}`} label="Corpo de exemplo" />
      </Panel>

      <Drawer
        open={form !== null}
        title={form?.id ? 'Webhook' : 'Criar webhook'}
        subtitle={form?.id ? <span className="mono">{form.url}</span> : 'O segredo para validar as entregas aparece uma vez, logo depois de criar.'}
        onClose={() => setForm(null)}
        wide
        footer={
          form ? (
            <>
              {form.id ? <ConfirmButton label="Excluir" confirmLabel="Excluir webhook" onConfirm={remove} small={false} /> : null}
              {form.id ? (
                <Button icon="play" busy={testing} onClick={test}>
                  Enviar teste
                </Button>
              ) : null}
              <span className="spacer" />
              <Button variant="primary" busy={busy} onClick={() => submit()}>
                {form.id ? 'Salvar' : 'Criar webhook'}
              </Button>
            </>
          ) : undefined
        }
      >
        {form ? (
          <form className="form" onSubmit={submit}>
            <Field label="Endereço de destino" htmlFor="wh-url" error={fieldErr.url} hint="Recebe um POST com Content-Type: application/json.">
              <input id="wh-url" className="mono" value={form.url} onChange={(e) => setForm({ ...form, url: e.target.value })} spellCheck={false} />
            </Field>
            <fieldset className={`event-grid${fieldErr.events ? ' field-error' : ''}`}>
              <legend>Eventos</legend>
              {evs.length === 0 && events.loading ? <Loading /> : null}
              {evs.map((ev) => (
                <label key={ev.id} className="check">
                  <input type="checkbox" checked={form.events.includes(ev.id)} onChange={() => toggleEvent(ev.id)} />
                  <span>
                    {ev.label}
                    <small className="mono">{ev.id}</small>
                  </span>
                </label>
              ))}
              {fieldErr.events ? <span className="field-msg">{fieldErr.events}</span> : null}
            </fieldset>
            <label className="check">
              <input type="checkbox" checked={form.active} onChange={(e) => setForm({ ...form, active: e.target.checked })} />
              <span>Ativo: enviar os eventos assim que acontecerem</span>
            </label>
            {form.id ? (
              <>
                <div className="hook-secret">
                  <span>Segredo de assinatura</span>
                  <ConfirmButton label="Gerar novo segredo" confirmLabel="Gerar e invalidar o atual" icon="refresh" onConfirm={rotate} />
                </div>
                <div className="deliveries">
                  <h3>Últimas entregas</h3>
                  {deliveries === null ? (
                    <Loading />
                  ) : deliveries.length === 0 ? (
                    <p className="muted">Nenhuma entrega ainda. Use Enviar teste para conferir o destino.</p>
                  ) : (
                    <table className="grid">
                      <thead>
                        <tr>
                          <th>Quando</th>
                          <th>Evento</th>
                          <th className="num">Status</th>
                          <th className="num">Tempo</th>
                        </tr>
                      </thead>
                      <tbody>
                        {deliveries.map((d) => (
                          <tr key={d.id} title={d.error}>
                            <td>{dateTime(d.at)}</td>
                            <td className="mono">{d.event}</td>
                            <td className="num">
                              <Badge tone={statusTone(d.status)}>{d.status || 'erro'}</Badge>
                            </td>
                            <td className="num">{ms(d.ms)}</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  )}
                </div>
              </>
            ) : null}
          </form>
        ) : null}
      </Drawer>
    </div>
  )
}

/* --------------------------------------------------------------- Prometheus */

function PrometheusTab({ info }: { info: IntegrationInfo }) {
  const [preview, setPreview] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const base = info.http_base || window.location.origin
  let target = 'localhost:8080'
  let scheme = 'http'
  try {
    const u = new URL(base)
    target = u.host
    scheme = u.protocol.replace(':', '')
  } catch {
    /* mantém o padrão */
  }

  const load = async () => {
    setLoading(true)
    setError(null)
    try {
      setPreview(await api.metricsText())
    } catch (e) {
      setError(errorMessage(e))
    } finally {
      setLoading(false)
    }
  }

  const yaml = `scrape_configs:
  - job_name: arca
    scrape_interval: 15s
    scheme: ${scheme}
    metrics_path: ${info.metrics_url}
    authorization:
      type: Bearer
      credentials: arca_...   # chave de acesso com papel Leitura
    static_configs:
      - targets: ['${target}']
`

  const samples = (preview ?? '')
    .split('\n')
    .filter((l) => l && !l.startsWith('#'))
  const shown = samples.slice(0, 40)

  return (
    <div className="integ-connect">
      <Panel title="Coleta">
        <p className="integ-note">
          O gateway expõe as métricas do servidor e as dele mesmo em <span className="mono">{base}{info.metrics_url}</span>, no formato de texto do Prometheus. A rota exige uma chave de acesso; crie uma com papel Leitura na aba Chaves de acesso.
        </p>
        <Snippet code={yaml} label="prometheus.yml" />
        <p className="integ-note">
          Sugestões de alerta: <code>arca_up == 0</code>, <code>arca_write_amplification &gt; 10</code> e <code>arca_sst_files{'{'}level="0"{'}'} &gt;= 8</code> (o L0 acumulando antes da compactação).
        </p>
      </Panel>
      <Panel
        title={preview ? <>Prévia <span className="muted">· {num(samples.length)} séries</span></> : 'Prévia'}
        actions={
          <Button small icon={preview ? 'refresh' : 'gauge'} busy={loading} onClick={load}>
            {preview ? 'Atualizar' : 'Carregar prévia'}
          </Button>
        }
      >
        {error ? <ErrorBox message={error} /> : null}
        {preview ? (
          <pre className="snippet-code metrics-preview">
            <code>
              {shown.join('\n')}
              {samples.length > shown.length ? `\n… e mais ${samples.length - shown.length}` : ''}
            </code>
          </pre>
        ) : !error ? (
          <p className="muted">Busca /metrics agora, com a sua sessão, para você ver exatamente o que o Prometheus vai receber.</p>
        ) : null}
      </Panel>
    </div>
  )
}

/* ------------------------------------------------------------------- página */

export function Integrations() {
  const { can } = useSession()
  const info = useAsync(() => api.integrations(), [])
  const options: [TabId, string][] = [
    ['conectar', 'Conectar'],
    ['chaves', 'Chaves de acesso'],
    ...(can('admin') ? ([['webhooks', 'Webhooks']] as [TabId, string][]) : []),
    ['prometheus', 'Prometheus'],
  ]
  const [tab, setTab] = useState<TabId>(() => {
    const saved = readPref(TAB_KEY)
    return options.some(([t]) => t === saved) ? (saved as TabId) : 'conectar'
  })
  const choose = (t: TabId) => {
    setTab(t)
    writePref(TAB_KEY, t)
  }
  const fallback: IntegrationInfo = {
    version: '',
    redis_host: window.location.hostname || 'localhost',
    redis_port: 7379,
    http_base: window.location.origin,
    openapi_url: '/api/openapi.json',
    metrics_url: '/metrics',
    requires_password: true,
  }
  const data = info.data ?? fallback

  return (
    <div className="stack integrations">
      <Tabs value={tab} onChange={choose} options={options} />
      {info.error && !info.data && (tab === 'conectar' || tab === 'prometheus') ? <ErrorBox message={info.error} onRetry={info.reload} /> : null}
      {tab === 'conectar' ? info.data || info.error ? <Connect info={data} /> : <Loading /> : null}
      {tab === 'chaves' ? <TokensTab /> : null}
      {tab === 'webhooks' && can('admin') ? <WebhooksTab /> : null}
      {tab === 'prometheus' ? info.data || info.error ? <PrometheusTab info={data} /> : <Loading /> : null}
    </div>
  )
}

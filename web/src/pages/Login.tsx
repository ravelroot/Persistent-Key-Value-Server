import { useState } from 'react'
import type { FormEvent } from 'react'
import { api, errorMessage, setToken } from '../api'
import { Logo } from '../components/Logo'
import { Button, Field } from '../components/ui'
import type { User } from '../types'

const demo = [
  ['admin@arca.dev', 'Administrador'],
  ['operador@arca.dev', 'Operador'],
  ['leitura@arca.dev', 'Leitura'],
]

export function Login({ onLogin }: { onLogin: (u: User) => void }) {
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true)
    setError(null)
    try {
      const res = await api.login(email, password)
      setToken(res.token)
      onLogin(res.user)
    } catch (err) {
      setError(errorMessage(err))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="login">
      <div className="login-art" aria-hidden="true">
        <div className="login-caption">
          <Logo size={36} />
          <p>Escritas entram na memtable, viram SSTables e descem em camadas até o fundo.</p>
        </div>
        <ol className="login-strata">
          <li className="ls-mem">
            <span>memtable</span>
          </li>
          {[0, 1, 2, 3, 4, 5, 6].map((l) => (
            <li key={l} className={`ls-l${l}`}>
              <span>L{l}</span>
            </li>
          ))}
        </ol>
      </div>
      <form className="login-card" onSubmit={submit}>
        <div className="login-brand">
          <span className="brand-mark brand-mark-lg">
            <Logo size={22} />
          </span>
          <div>
            <h1>Arca</h1>
            <p>Painel do banco de dados</p>
          </div>
        </div>
        <Field label="E-mail" htmlFor="login-email">
          <input id="login-email" type="email" autoComplete="username" value={email} onChange={(e) => setEmail(e.target.value)} required />
        </Field>
        <Field label="Senha" htmlFor="login-senha">
          <input id="login-senha" type="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} required />
        </Field>
        {error ? (
          <div className="error-box" role="alert">
            {error}
          </div>
        ) : null}
        <Button type="submit" variant="primary" busy={busy} className="login-submit">
          Entrar
        </Button>
        <div className="login-demo">
          <span>Entre com uma conta de demonstração (senha Arca@2026):</span>
          <div>
            {demo.map(([mail, label]) => (
              <button
                key={mail}
                type="button"
                className="chip"
                onClick={() => {
                  setEmail(mail)
                  setPassword('Arca@2026')
                }}
              >
                {label}
              </button>
            ))}
          </div>
        </div>
      </form>
    </div>
  )
}

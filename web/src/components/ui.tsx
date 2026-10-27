import { useEffect, useRef, useState } from 'react'
import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { Icon } from './Icons'
import type { IconName } from './Icons'

type Variant = 'primary' | 'ghost' | 'danger' | 'quiet'

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: Variant
  icon?: IconName
  busy?: boolean
  small?: boolean
}

export function Button({ variant = 'ghost', icon, busy, small, children, className, disabled, ...rest }: ButtonProps) {
  return (
    <button
      type="button"
      className={`btn btn-${variant}${small ? ' btn-sm' : ''}${className ? ' ' + className : ''}`}
      disabled={disabled || busy}
      aria-busy={busy || undefined}
      {...rest}
    >
      {busy ? <span className="spinner" aria-hidden="true" /> : icon ? <Icon name={icon} size={small ? 15 : 17} /> : null}
      {children ? <span>{children}</span> : null}
    </button>
  )
}

export function IconButton({ icon, label, onClick, disabled }: { icon: IconName; label: string; onClick?: () => void; disabled?: boolean }) {
  return (
    <button type="button" className="icon-btn" aria-label={label} title={label} onClick={onClick} disabled={disabled}>
      <Icon name={icon} size={17} />
    </button>
  )
}

export function Badge({ tone = 'neutral', children }: { tone?: 'neutral' | 'ok' | 'warn' | 'error' | 'accent'; children: ReactNode }) {
  return <span className={`badge badge-${tone}`}>{children}</span>
}

export function Panel({ title, actions, children, className, flush }: { title?: ReactNode; actions?: ReactNode; children: ReactNode; className?: string; flush?: boolean }) {
  return (
    <section className={`panel${flush ? ' panel-flush' : ''}${className ? ' ' + className : ''}`}>
      {title || actions ? (
        <header className="panel-head">
          {title ? <h2>{title}</h2> : <span />}
          {actions ? <div className="panel-actions">{actions}</div> : null}
        </header>
      ) : null}
      <div className="panel-body">{children}</div>
    </section>
  )
}

export function Empty({ title, children }: { title: string; children?: ReactNode }) {
  return (
    <div className="empty">
      <strong>{title}</strong>
      {children ? <p>{children}</p> : null}
    </div>
  )
}

export function ErrorBox({ message, onRetry }: { message: string; onRetry?: () => void }) {
  return (
    <div className="error-box" role="alert">
      <span>{message}</span>
      {onRetry ? (
        <Button small icon="refresh" onClick={onRetry}>
          Tentar de novo
        </Button>
      ) : null}
    </div>
  )
}

export function Loading({ label = 'Carregando…' }: { label?: string }) {
  return (
    <div className="loading">
      <span className="spinner" aria-hidden="true" />
      <span>{label}</span>
    </div>
  )
}

export function Field({ label, hint, error, children, htmlFor }: { label: string; hint?: ReactNode; error?: string; children: ReactNode; htmlFor?: string }) {
  return (
    <div className={`field${error ? ' field-error' : ''}`}>
      <label htmlFor={htmlFor}>{label}</label>
      {children}
      {error ? <span className="field-msg">{error}</span> : hint ? <span className="field-hint">{hint}</span> : null}
    </div>
  )
}

export function Drawer({ open, title, subtitle, onClose, children, footer, wide }: { open: boolean; title: string; subtitle?: ReactNode; onClose: () => void; children: ReactNode; footer?: ReactNode; wide?: boolean }) {
  const ref = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!open) return
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose()
    }
    window.addEventListener('keydown', onKey)
    const prev = document.activeElement as HTMLElement | null
    window.setTimeout(() => {
      const el = ref.current?.querySelector<HTMLElement>('input, textarea, select, button')
      el?.focus()
    }, 30)
    return () => {
      window.removeEventListener('keydown', onKey)
      prev?.focus?.()
    }
  }, [open, onClose])
  if (!open) return null
  return (
    <div className="drawer-wrap" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className={`drawer${wide ? ' drawer-wide' : ''}`} role="dialog" aria-modal="true" aria-label={title} ref={ref}>
        <header className="drawer-head">
          <div>
            <h2>{title}</h2>
            {subtitle ? <div className="drawer-sub">{subtitle}</div> : null}
          </div>
          <IconButton icon="close" label="Fechar" onClick={onClose} />
        </header>
        <div className="drawer-body">{children}</div>
        {footer ? <footer className="drawer-foot">{footer}</footer> : null}
      </div>
    </div>
  )
}

export function ConfirmButton({ label, confirmLabel = 'Confirmar', onConfirm, icon = 'trash', busy, disabled, small = true }: { label: string; confirmLabel?: string; onConfirm: () => void; icon?: IconName; busy?: boolean; disabled?: boolean; small?: boolean }) {
  const [armed, setArmed] = useState(false)
  useEffect(() => {
    if (!armed) return
    const t = window.setTimeout(() => setArmed(false), 4000)
    return () => window.clearTimeout(t)
  }, [armed])
  if (armed) {
    return (
      <span className="confirm">
        <Button small={small} variant="danger" busy={busy} onClick={() => { setArmed(false); onConfirm() }}>
          {confirmLabel}
        </Button>
        <Button small={small} variant="quiet" onClick={() => setArmed(false)}>
          Cancelar
        </Button>
      </span>
    )
  }
  return (
    <Button small={small} variant="ghost" icon={icon} busy={busy} disabled={disabled} onClick={() => setArmed(true)}>
      {label}
    </Button>
  )
}

export function Stat({ label, value, hint, tone }: { label: string; value: ReactNode; hint?: ReactNode; tone?: 'ok' | 'warn' | 'error' }) {
  return (
    <div className={`readout${tone ? ' readout-' + tone : ''}`}>
      <span className="readout-label">{label}</span>
      <span className="readout-value">{value}</span>
      {hint ? <span className="readout-hint">{hint}</span> : null}
    </div>
  )
}

export function KV({ items }: { items: [string, ReactNode][] }) {
  return (
    <dl className="kv">
      {items.map(([k, v]) => (
        <div key={k}>
          <dt>{k}</dt>
          <dd>{v}</dd>
        </div>
      ))}
    </dl>
  )
}

export function Tabs<T extends string>({ value, options, onChange }: { value: T; options: [T, string][]; onChange: (v: T) => void }) {
  return (
    <div className="tabs" role="tablist">
      {options.map(([v, label]) => (
        <button key={v} type="button" role="tab" aria-selected={v === value} className={v === value ? 'tab active' : 'tab'} onClick={() => onChange(v)}>
          {label}
        </button>
      ))}
    </div>
  )
}

export function copyText(text: string): Promise<boolean> {
  try {
    return navigator.clipboard.writeText(text).then(
      () => true,
      () => false,
    )
  } catch {
    return Promise.resolve(false)
  }
}

import { useEffect, useMemo, useRef, useState } from 'react'
import { Icon } from './Icons'
import type { IconName } from './Icons'

export interface PaletteItem {
  id: string
  group: string
  label: string
  hint?: string
  icon: IconName
  keywords?: string
  shortcut?: string[]
  run: () => void
}

function normalize(s: string): string {
  return s
    .normalize('NFD')
    .replace(/[̀-ͯ]/g, '')
    .toLowerCase()
}

/** Pontua o item: começo do rótulo vale mais que começo de palavra, que vale mais que palavra-chave. */
function score(item: PaletteItem, q: string): number {
  if (!q) return 1
  const label = normalize(item.label)
  if (label.startsWith(q)) return 4
  if (label.split(/\s+/).some((w) => w.startsWith(q))) return 3
  if (label.includes(q)) return 2
  if (normalize(`${item.keywords ?? ''} ${item.hint ?? ''} ${item.group}`).includes(q)) return 1
  return 0
}

export function CommandPalette({ open, items, onClose }: { open: boolean; items: PaletteItem[]; onClose: () => void }) {
  const [query, setQuery] = useState('')
  const [active, setActive] = useState(0)
  const inputRef = useRef<HTMLInputElement>(null)
  const listRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) return
    setQuery('')
    setActive(0)
    const prev = document.activeElement as HTMLElement | null
    window.setTimeout(() => inputRef.current?.focus(), 10)
    return () => prev?.focus?.()
  }, [open])

  const results = useMemo(() => {
    const q = normalize(query.trim())
    return items
      .map((item, i) => ({ item, s: score(item, q), i }))
      .filter((r) => r.s > 0)
      .sort((a, b) => b.s - a.s || a.i - b.i)
      .map((r) => r.item)
  }, [items, query])

  useEffect(() => {
    setActive(0)
  }, [query])

  useEffect(() => {
    listRef.current?.querySelector<HTMLElement>(`[data-index="${active}"]`)?.scrollIntoView({ block: 'nearest' })
  }, [active])

  if (!open) return null

  const choose = (item: PaletteItem | undefined) => {
    if (!item) return
    onClose()
    item.run()
  }

  const onKey = (e: React.KeyboardEvent) => {
    if (e.key === 'ArrowDown') {
      e.preventDefault()
      setActive((a) => Math.min(results.length - 1, a + 1))
    } else if (e.key === 'ArrowUp') {
      e.preventDefault()
      setActive((a) => Math.max(0, a - 1))
    } else if (e.key === 'Enter') {
      e.preventDefault()
      choose(results[active])
    } else if (e.key === 'Escape') {
      e.preventDefault()
      onClose()
    }
  }

  let lastGroup = ''
  return (
    <div className="palette-wrap" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="palette" role="dialog" aria-modal="true" aria-label="Ir para" onKeyDown={onKey}>
        <div className="palette-input">
          <Icon name="search" size={18} />
          <input
            ref={inputRef}
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder="Procure uma tela ou uma ação"
            aria-label="Procurar"
            role="combobox"
            aria-expanded="true"
            aria-controls="palette-list"
            aria-activedescendant={results[active] ? `pal-${results[active].id}` : undefined}
          />
          <kbd>Esc</kbd>
        </div>
        <div className="palette-list" id="palette-list" role="listbox" ref={listRef}>
          {results.length === 0 ? <p className="palette-empty">Nada com “{query}”. Tente o nome de uma tela, como “chaves” ou “webhook”.</p> : null}
          {results.map((item, i) => {
            const head = item.group !== lastGroup ? item.group : null
            lastGroup = item.group
            return (
              <div key={item.id}>
                {head && !query ? <div className="palette-group">{head}</div> : null}
                <div
                  id={`pal-${item.id}`}
                  role="option"
                  aria-selected={i === active}
                  data-index={i}
                  className={`palette-item${i === active ? ' active' : ''}`}
                  onMouseMove={() => setActive(i)}
                  onClick={() => choose(item)}
                >
                  <Icon name={item.icon} size={17} />
                  <span className="palette-text">
                    <b>{item.label}</b>
                    {item.hint ? <small>{item.hint}</small> : null}
                  </span>
                  {item.shortcut ? (
                    <span className="palette-keys">
                      {item.shortcut.map((k) => (
                        <kbd key={k}>{k}</kbd>
                      ))}
                    </span>
                  ) : null}
                </div>
              </div>
            )
          })}
        </div>
        <footer className="palette-foot">
          <span>
            <kbd>↑</kbd>
            <kbd>↓</kbd> escolher
          </span>
          <span>
            <kbd>Enter</kbd> abrir
          </span>
          <span>
            <kbd>g</kbd> + letra navega sem abrir esta caixa
          </span>
        </footer>
      </div>
    </div>
  )
}

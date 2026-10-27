import { Drawer } from './ui'

const general: { keys: string[]; label: string }[] = [
  { keys: ['Ctrl', 'K'], label: 'Ir para uma tela ou ação' },
  { keys: ['?'], label: 'Mostrar esta lista' },
  { keys: ['Ctrl', 'Enter'], label: 'Executar no Console SQL e em Comandos' },
  { keys: ['Esc'], label: 'Fechar painéis e diálogos' },
]

export function Shortcuts({ open, onClose, items }: { open: boolean; onClose: () => void; items: { keys: string[]; label: string }[] }) {
  return (
    <Drawer open={open} title="Atalhos de teclado" subtitle="Funcionam fora de campos de texto. No Mac, use ⌘ no lugar de Ctrl." onClose={onClose}>
      <div className="shortcuts">
        <h3>Gerais</h3>
        <dl>
          {general.map((s) => (
            <div key={s.label}>
              <dt>{s.label}</dt>
              <dd>
                {s.keys.map((k) => (
                  <kbd key={k}>{k}</kbd>
                ))}
              </dd>
            </div>
          ))}
        </dl>
        <h3>Navegar: aperte g e depois a letra</h3>
        <dl>
          {items.map((s) => (
            <div key={s.label}>
              <dt>{s.label}</dt>
              <dd>
                {s.keys.map((k) => (
                  <kbd key={k}>{k}</kbd>
                ))}
              </dd>
            </div>
          ))}
        </dl>
      </div>
    </Drawer>
  )
}

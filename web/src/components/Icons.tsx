import type { ReactNode } from 'react'

export type IconName =
  | 'overview'
  | 'keys'
  | 'tables'
  | 'sql'
  | 'terminal'
  | 'layers'
  | 'clients'
  | 'settings'
  | 'users'
  | 'account'
  | 'sun'
  | 'moon'
  | 'logout'
  | 'search'
  | 'plus'
  | 'refresh'
  | 'trash'
  | 'edit'
  | 'close'
  | 'clock'
  | 'play'
  | 'menu'
  | 'chevron'
  | 'lock'
  | 'copy'
  | 'download'
  | 'plug'
  | 'upload'
  | 'webhook'
  | 'token'
  | 'command'
  | 'check'
  | 'bookmark'
  | 'history'
  | 'shield'
  | 'external'
  | 'gauge'
  | 'code'

const paths: Record<IconName, ReactNode> = {
  overview: (
    <>
      <path d="M3 13h4v8H3zM10 8h4v13h-4zM17 3h4v18h-4z" />
    </>
  ),
  keys: (
    <>
      <circle cx="8" cy="12" r="4" />
      <path d="M12 12h9M18 12v4M15 12v3" />
    </>
  ),
  tables: (
    <>
      <rect x="3" y="4" width="18" height="16" rx="1.5" />
      <path d="M3 9h18M3 14.5h18M9.5 9v11" />
    </>
  ),
  sql: (
    <>
      <ellipse cx="12" cy="5.5" rx="7.5" ry="2.5" />
      <path d="M4.5 5.5v13c0 1.4 3.4 2.5 7.5 2.5s7.5-1.1 7.5-2.5v-13M4.5 12c0 1.4 3.4 2.5 7.5 2.5s7.5-1.1 7.5-2.5" />
    </>
  ),
  terminal: (
    <>
      <rect x="3" y="4" width="18" height="16" rx="1.5" />
      <path d="M7 9l3 3-3 3M12.5 15H17" />
    </>
  ),
  layers: (
    <>
      <path d="M8 4h8M5.5 9.5h13M3 15h18M3 20.5h18" />
    </>
  ),
  clients: (
    <>
      <rect x="3" y="4" width="18" height="6" rx="1" />
      <rect x="3" y="14" width="18" height="6" rx="1" />
      <path d="M7 7h.01M7 17h.01" />
    </>
  ),
  settings: (
    <>
      <path d="M4 6h10M18 6h2M4 12h3M11 12h9M4 18h12M20 18h0" />
      <circle cx="16" cy="6" r="2" />
      <circle cx="9" cy="12" r="2" />
      <circle cx="18" cy="18" r="2" />
    </>
  ),
  users: (
    <>
      <circle cx="9" cy="8" r="3.5" />
      <path d="M2.5 20c.6-3.6 3.2-5.5 6.5-5.5s5.9 1.9 6.5 5.5M16 4.8a3.5 3.5 0 0 1 0 6.4M18 14.8c1.9.6 3.1 2.3 3.5 5.2" />
    </>
  ),
  account: (
    <>
      <circle cx="12" cy="8.5" r="4" />
      <path d="M4 21c.8-4 4-6 8-6s7.2 2 8 6" />
    </>
  ),
  sun: (
    <>
      <circle cx="12" cy="12" r="4" />
      <path d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M5.3 18.7l1.4-1.4M17.3 6.7l1.4-1.4" />
    </>
  ),
  moon: (
    <>
      <path d="M20 14.5A8.5 8.5 0 0 1 9.5 4a8.5 8.5 0 1 0 10.5 10.5z" />
    </>
  ),
  logout: (
    <>
      <path d="M14 4h5a1 1 0 0 1 1 1v14a1 1 0 0 1-1 1h-5M10 16l4-4-4-4M14 12H3" />
    </>
  ),
  search: (
    <>
      <circle cx="10.5" cy="10.5" r="6.5" />
      <path d="M15.5 15.5L21 21" />
    </>
  ),
  plus: (
    <>
      <path d="M12 5v14M5 12h14" />
    </>
  ),
  refresh: (
    <>
      <path d="M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7" />
    </>
  ),
  trash: (
    <>
      <path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13M10 11v6M14 11v6" />
    </>
  ),
  edit: (
    <>
      <path d="M4 20h4L19 9l-4-4L4 16v4zM13 7l4 4" />
    </>
  ),
  close: (
    <>
      <path d="M6 6l12 12M18 6L6 18" />
    </>
  ),
  clock: (
    <>
      <circle cx="12" cy="12" r="8.5" />
      <path d="M12 7v5l3 2" />
    </>
  ),
  play: (
    <>
      <path d="M7 4.5v15l12-7.5z" />
    </>
  ),
  menu: (
    <>
      <path d="M4 7h16M4 12h16M4 17h16" />
    </>
  ),
  chevron: (
    <>
      <path d="M9 6l6 6-6 6" />
    </>
  ),
  lock: (
    <>
      <rect x="5" y="10.5" width="14" height="10" rx="1.5" />
      <path d="M8 10.5V8a4 4 0 0 1 8 0v2.5" />
    </>
  ),
  copy: (
    <>
      <rect x="8" y="8" width="12" height="12" rx="1.5" />
      <path d="M16 8V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h3" />
    </>
  ),
  download: (
    <>
      <path d="M12 4v11M7 10l5 5 5-5M4 20h16" />
    </>
  ),
  plug: (
    <>
      <path d="M9 3v4M15 3v4M6.5 7h11v3.5a5.5 5.5 0 0 1-11 0V7zM12 16v5" />
    </>
  ),
  upload: (
    <>
      <path d="M12 16V5M7 10l5-5 5 5M4 20h16" />
    </>
  ),
  webhook: (
    <>
      <circle cx="6" cy="17" r="2.5" />
      <circle cx="18" cy="17" r="2.5" />
      <circle cx="12" cy="6" r="2.5" />
      <path d="M10.8 8.2L7.2 14.8M8.5 17h7M13.2 8.2l3.6 6.6" />
    </>
  ),
  token: (
    <>
      <rect x="3" y="7" width="18" height="10" rx="1.5" />
      <path d="M7 12h.01M10.5 12h.01M14 12h3" />
    </>
  ),
  command: (
    <>
      <rect x="3" y="5" width="18" height="14" rx="1.5" />
      <path d="M7 15l3-3-3-3M12 15h5" />
    </>
  ),
  check: (
    <>
      <path d="M5 12.5l4.5 4.5L19 7.5" />
    </>
  ),
  bookmark: (
    <>
      <path d="M6.5 4h11v16.5L12 16l-5.5 4.5z" />
    </>
  ),
  history: (
    <>
      <path d="M4 12a8 8 0 1 0 2.3-5.7M4 4v4.5h4.5" />
      <path d="M12 8v4.5l3 1.8" />
    </>
  ),
  shield: (
    <>
      <path d="M12 3l7.5 3v5.5c0 4.6-3.2 8.2-7.5 9.5-4.3-1.3-7.5-4.9-7.5-9.5V6z" />
      <path d="M8.5 12l2.5 2.5 4.5-5" />
    </>
  ),
  external: (
    <>
      <path d="M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5" />
    </>
  ),
  gauge: (
    <>
      <path d="M3.5 16a8.5 8.5 0 1 1 17 0" />
      <path d="M12 16l4-5M3.5 16h2M18.5 16h2M12 7.5v2" />
    </>
  ),
  code: (
    <>
      <path d="M8.5 7L3.5 12l5 5M15.5 7l5 5-5 5M13.5 4.5l-3 15" />
    </>
  ),
}

export function Icon({ name, size = 18 }: { name: IconName; size?: number }) {
  return (
    <svg
      className="icon"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.7"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {paths[name]}
    </svg>
  )
}

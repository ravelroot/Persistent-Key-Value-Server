/** CSV no formato RFC 4180: vírgula, aspas duplas e quebra de linha dentro de aspas. */

export function parseCSV(text: string): string[][] {
  const rows: string[][] = []
  let row: string[] = []
  let field = ''
  let quoted = false
  let i = 0
  const src = text.charCodeAt(0) === 0xfeff ? text.slice(1) : text
  while (i < src.length) {
    const ch = src[i]
    if (quoted) {
      if (ch === '"') {
        if (src[i + 1] === '"') {
          field += '"'
          i += 2
          continue
        }
        quoted = false
        i++
        continue
      }
      field += ch
      i++
      continue
    }
    if (ch === '"') {
      quoted = true
      i++
      continue
    }
    if (ch === ',') {
      row.push(field)
      field = ''
      i++
      continue
    }
    if (ch === '\r' || ch === '\n') {
      row.push(field)
      field = ''
      if (row.length > 1 || row[0] !== '') rows.push(row)
      row = []
      if (ch === '\r' && src[i + 1] === '\n') i++
      i++
      continue
    }
    field += ch
    i++
  }
  if (field !== '' || row.length) {
    row.push(field)
    if (row.length > 1 || row[0] !== '') rows.push(row)
  }
  return rows
}

function escapeCell(v: unknown): string {
  if (v === null || v === undefined) return ''
  const s = typeof v === 'string' ? v : String(v)
  return /[",\r\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s
}

export function toCSV(columns: string[], rows: unknown[][]): string {
  const out = [columns.map(escapeCell).join(',')]
  for (const r of rows) out.push(r.map(escapeCell).join(','))
  return out.join('\r\n') + '\r\n'
}

/** Oferece um texto já pronto como arquivo para salvar. */
export function saveText(text: string, name: string, type = 'text/csv;charset=utf-8'): void {
  const blob = new Blob(['﻿' + text], { type })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = name
  document.body.appendChild(a)
  a.click()
  a.remove()
  window.setTimeout(() => URL.revokeObjectURL(url), 1000)
}

export function readFile(file: File): Promise<string> {
  return new Promise((resolve, reject) => {
    const r = new FileReader()
    r.onload = () => resolve(String(r.result ?? ''))
    r.onerror = () => reject(new Error('Não foi possível ler o arquivo escolhido.'))
    r.readAsText(file)
  })
}

import { useState } from 'react'
import { api } from '../api'
import { bytes, dateTime, ago, ms, num } from '../format'
import { useAsync, useSession, useToast } from '../state'
import type { VerifyResult } from '../types'
import { Icon } from './Icons'
import { Badge, Button, ConfirmButton, Empty, ErrorBox, Loading, Panel } from './ui'

/** Checkpoints gravados em disco e verificação de integridade das SSTables vivas. */
export function Checkpoints() {
  const { can } = useSession()
  const toast = useToast()
  const list = useAsync(() => api.checkpoints(), [])
  const [creating, setCreating] = useState(false)
  const [verifying, setVerifying] = useState(false)
  const [verify, setVerify] = useState<VerifyResult | null>(null)
  const [verifyErr, setVerifyErr] = useState<string | null>(null)

  const create = async () => {
    setCreating(true)
    try {
      const r = await api.checkpoint()
      toast.ok(r?.message || 'Checkpoint criado.')
      list.reload()
    } catch (e) {
      toast.error(e)
    } finally {
      setCreating(false)
    }
  }

  const remove = async (name: string) => {
    try {
      await api.checkpointDelete(name)
      toast.ok(`Checkpoint ${name} excluído.`)
      list.reload()
    } catch (e) {
      toast.error(e)
    }
  }

  const runVerify = async () => {
    setVerifying(true)
    setVerifyErr(null)
    try {
      const r = await api.verify()
      setVerify(r)
      if (r.corrupt.length) toast.push('error', `${num(r.corrupt.length)} ${r.corrupt.length === 1 ? 'SSTable com checksum inválido' : 'SSTables com checksum inválido'}.`)
      else toast.ok(`Integridade conferida: ${num(r.ok)} SSTables sem erro.`)
    } catch (e) {
      setVerifyErr(e instanceof Error ? e.message : 'Falha ao verificar.')
    } finally {
      setVerifying(false)
    }
  }

  const items = list.data ?? []
  const total = items.reduce((acc, c) => acc + c.bytes, 0)

  return (
    <div className="checkpoints">
      <Panel
        flush
        title={
          <>
            Checkpoints <span className="muted">· {num(items.length)} em disco{items.length ? `, ${bytes(total)}` : ''}</span>
          </>
        }
        actions={
          can('operator') ? (
            <Button small icon="plus" busy={creating} onClick={create}>
              Criar checkpoint
            </Button>
          ) : undefined
        }
      >
        {list.error && !list.data ? (
          <div className="side-pad">
            <ErrorBox message={list.error} onRetry={list.reload} />
          </div>
        ) : !list.data ? (
          <Loading />
        ) : items.length === 0 ? (
          <Empty title="Nenhum checkpoint ainda">
            Um checkpoint é uma cópia consistente do banco, pronta para abrir como backup. Crie o primeiro antes de uma mudança grande.
          </Empty>
        ) : (
          <div className="table-wrap">
            <table className="grid">
              <thead>
                <tr>
                  <th>Nome</th>
                  <th>Criado</th>
                  <th className="num">Arquivos</th>
                  <th className="num">Tamanho</th>
                  {can('admin') ? <th aria-label="Ações" /> : null}
                </tr>
              </thead>
              <tbody>
                {items.map((c) => (
                  <tr key={c.name}>
                    <td className="mono">{c.name}</td>
                    <td title={dateTime(c.created_ms)}>{ago(c.created_ms)}</td>
                    <td className="num">{num(c.files)}</td>
                    <td className="num">{bytes(c.bytes)}</td>
                    {can('admin') ? (
                      <td className="num">
                        <ConfirmButton label="Excluir" confirmLabel="Excluir checkpoint" onConfirm={() => remove(c.name)} />
                      </td>
                    ) : null}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Panel>

      <Panel
        title="Integridade"
        actions={
          can('operator') ? (
            <Button small icon="shield" busy={verifying} onClick={runVerify}>
              Verificar integridade
            </Button>
          ) : undefined
        }
      >
        {verifyErr ? <ErrorBox message={verifyErr} /> : null}
        {verify ? (
          <div className="verify">
            <div className={`verify-head${verify.corrupt.length ? ' verify-bad' : ' verify-ok'}`}>
              <Icon name={verify.corrupt.length ? 'close' : 'check'} size={18} />
              <strong>
                {verify.corrupt.length
                  ? `${num(verify.corrupt.length)} de ${num(verify.files)} SSTables com erro`
                  : `${num(verify.ok)} de ${num(verify.files)} SSTables íntegras`}
              </strong>
              <span className="muted">
                {bytes(verify.bytes)} lidos em {ms(verify.ms)}
              </span>
            </div>
            {verify.corrupt.length ? (
              <>
                <ul className="verify-list">
                  {verify.corrupt.map((c) => (
                    <li key={c.file}>
                      <span className="mono">{c.file}</span>
                      <Badge tone="error">{c.error}</Badge>
                    </li>
                  ))}
                </ul>
                <p className="muted">
                  Restaure a partir de um checkpoint ou rode <code>arca-inspect check</code> com o servidor parado para ver os blocos afetados.
                </p>
              </>
            ) : null}
          </div>
        ) : (
          <p className="muted verify-intro">
            Relê cada bloco das SSTables vivas e confere o CRC32C, sem parar o servidor. Rode depois de uma queda de energia ou antes de copiar o diretório de dados.
          </p>
        )}
      </Panel>
    </div>
  )
}

export type Role = 'reader' | 'operator' | 'admin'

export interface User {
  email: string
  name: string
  role: Role
  created_at?: number
  locked?: boolean
}

export interface LoginResponse {
  token: string
  user: User
  expires_at: number
}

export interface SSTable {
  number: number
  bytes: number
  smallest: string
  largest: string
  entries: number
  deletions: number
}

export interface Level {
  level: number
  files: number
  bytes: number
  max_bytes: number
  score: number
  tables: SSTable[]
}

export interface Compaction {
  from_level: number
  to_level: number
  inputs: number
  outputs: number
  bytes_read: number
  bytes_written: number
  ms: number
  at: number
  reason: string
}

export interface EngineStats {
  last_sequence: number
  memtable_bytes: number
  memtable_entries: number
  immutable_bytes: number
  wal_number: number
  wal_bytes: number
  sst_files: number
  sst_bytes: number
  flushes: number
  compactions: number
  compaction_bytes_read: number
  compaction_bytes_written: number
  user_bytes_written: number
  write_amplification: number
  write_stalls: number
  bloom_checks: number
  bloom_useful: number
  block_cache_bytes: number
  block_cache_capacity: number
  block_cache_hits: number
  block_cache_misses: number
  compacting: boolean
  snapshots: number
  estimated_keys: number
}

export interface Overview {
  server: {
    version: string
    port: number
    uptime_s: number
    io_threads: number
    worker_threads: number
    appendfsync: string
    dir: string
  }
  clients: { connected: number; total: number; rejected: number }
  commands: {
    total: number
    ops_per_sec: number
    reads: number
    writes: number
    keyspace_hits: number
    keyspace_misses: number
    expired_keys: number
    net_input_bytes: number
    net_output_bytes: number
  }
  engine: EngineStats
  levels: Level[]
  recent_compactions: Compaction[]
  keyspace: { tables: number }
  tables: number
}

export interface Storage {
  levels: Level[]
  engine: EngineStats
  recent_compactions: Compaction[]
}

export interface Metrics {
  t: number
  ops_per_sec: number
  reads_per_sec: number
  writes_per_sec: number
  memtable_bytes: number
  immutable_bytes: number
  sst_bytes: number
  sst_files: number
  clients: number
  cache_hit_ratio: number
  compacting: boolean
  level_files: number[]
  write_amplification: number
}

export interface KeyItem {
  key: string
  type: string
  ttl_ms: number
  size: number
}

export interface KeyPage {
  cursor: string
  keys: KeyItem[]
}

export interface KeyValue {
  key: string
  value: string
  ttl_ms: number
  size: number
  binary?: boolean
}

export type ColumnType = 'INT' | 'REAL' | 'TEXT' | 'BOOL'

export interface Column {
  name: string
  type: ColumnType
  primary_key: boolean
  not_null: boolean
  default: unknown
  auto_increment: boolean
}

export interface Table {
  name: string
  columns: Column[]
  indexes: { name: string; column: string }[]
  rows: number
}

export type Cell = string | number | boolean | null

export interface RowPage {
  columns: string[]
  types: string[]
  rows: Cell[][]
  total: number
  primary_key: string
}

export interface SQLRows {
  kind: 'rows'
  columns: string[]
  types: string[]
  rows: Cell[][]
  plan?: string
  ms: number
}

export interface SQLCommand {
  kind: 'command'
  tag: string
  affected: number
  ms: number
}

export type SQLResult = SQLRows | SQLCommand

export type Reply = string | number | null | Reply[] | { error: string }

export interface CommandResult {
  reply: Reply
  ms: number
}

export interface ClientInfo {
  id: number
  addr: string
  name: string
  age_s: number
  idle_s: number
  cmd: string
}

export interface SlowEntry {
  id: number
  at: number
  micros: number
  args: string[]
  addr: string
}

export interface ConfigEntry {
  name: string
  value: string
}

export interface AuditEntry {
  id: number
  at: number
  email: string
  action: string
  detail: string
}

export interface Health {
  status: string
  arca: 'ok' | 'down'
  ready: boolean
}

export interface ApiToken {
  id: string
  name: string
  role: Role
  prefix: string
  owner: string
  created_at: number
  last_used_at: number | null
  expires_at: number | null
}

export interface TokenCreated {
  token: string
  info: ApiToken
}

export interface WebhookEvent {
  id: string
  label: string
}

export interface Webhook {
  id: string
  url: string
  events: string[]
  active: boolean
  created_at: number
  last_status: number | null
  last_delivery_at: number | null
  failures: number
}

export interface WebhookSaved {
  webhook: Webhook
  secret?: string
}

export interface WebhookTest {
  status: number
  ms: number
  error?: string
}

export interface WebhookDelivery {
  id: string
  at: number
  event: string
  status: number
  ms: number
  error?: string
}

export interface KeyExportItem {
  key: string
  value: string
  ttl_ms?: number | null
}

export interface KeyImportResult {
  imported: number
  skipped: number
  failed: number
}

export interface TableImportResult {
  inserted: number
  failed: number
  errors: { line: number; message: string }[]
}

export interface SavedQuery {
  id: string
  name: string
  query: string
  owner: string
  shared: boolean
  created_at: number
}

export interface Checkpoint {
  name: string
  created_ms: number
  bytes: number
  files: number
}

export interface VerifyResult {
  files: number
  bytes: number
  ok: number
  corrupt: { file: string; error: string }[]
  ms: number
}

export interface Integrations {
  version: string
  redis_host: string
  redis_port: number
  http_base: string
  openapi_url: string
  metrics_url: string
  requires_password: boolean
}

package api

import (
	"net/http"
	"regexp"
	"strings"
	"sync"
)

type opDoc struct {
	method   string
	path     string
	tag      string
	summary  string
	role     string
	request  string
	response string
	status   string
	query    []string
}

var routeDocs = []opDoc{
	{"post", "/api/auth/login", "Acesso", "Entra com e-mail e senha e recebe um token", "", "LoginRequest", "LoginResponse", "200", nil},
	{"post", "/api/auth/logout", "Acesso", "Encerra a sessão atual", "reader", "", "", "204", nil},
	{"get", "/api/auth/me", "Acesso", "Usuário da sessão", "reader", "", "User", "200", nil},
	{"put", "/api/auth/me/password", "Acesso", "Troca a própria senha", "reader", "PasswordChange", "", "204", nil},
	{"get", "/api/overview", "Monitoramento", "Estatísticas completas do servidor (ARCA.STATS) e número de tabelas", "reader", "", "Stats", "200", nil},
	{"get", "/api/events", "Monitoramento", "Fluxo SSE com o evento metrics a cada segundo", "reader", "", "", "200", []string{"token"}},
	{"get", "/api/keys", "Chaves", "Lista chaves por padrão com cursor", "reader", "", "KeyPage", "200", []string{"match", "cursor", "count"}},
	{"get", "/api/keys/value", "Chaves", "Lê o valor e o TTL de uma chave", "reader", "", "KeyValue", "200", []string{"key"}},
	{"put", "/api/keys/value", "Chaves", "Grava uma chave, com TTL opcional", "operator", "KeyPut", "", "204", nil},
	{"delete", "/api/keys/value", "Chaves", "Apaga uma chave", "operator", "", "", "204", []string{"key"}},
	{"post", "/api/keys/expire", "Chaves", "Define ou remove o TTL de uma chave", "operator", "KeyExpire", "", "204", nil},
	{"get", "/api/tables", "Tabelas", "Lista as tabelas, colunas e índices", "reader", "", "TableList", "200", nil},
	{"get", "/api/tables/{name}/rows", "Tabelas", "Linhas paginadas com ordenação e filtro", "reader", "", "RowPage", "200", []string{"limit", "offset", "order", "dir", "filter"}},
	{"post", "/api/tables/{name}/rows", "Tabelas", "Insere uma linha", "operator", "RowWrite", "Message", "201", nil},
	{"put", "/api/tables/{name}/rows", "Tabelas", "Altera uma linha pela chave primária", "operator", "RowWrite", "", "204", nil},
	{"delete", "/api/tables/{name}/rows", "Tabelas", "Apaga uma linha pela chave primária", "operator", "", "", "204", []string{"key"}},
	{"post", "/api/sql", "SQL", "Executa uma instrução SQL (leitura para reader)", "reader", "SQLRequest", "SQLResult", "200", nil},
	{"post", "/api/command", "Administração", "Envia um comando cru ao servidor", "admin", "CommandRequest", "CommandResult", "200", nil},
	{"get", "/api/storage", "Armazenamento", "Níveis, SSTables e compactações recentes", "reader", "", "Storage", "200", nil},
	{"post", "/api/storage/flush", "Armazenamento", "Grava a memtable em disco", "operator", "", "Message", "200", nil},
	{"post", "/api/storage/compact", "Armazenamento", "Compacta todos os níveis", "operator", "", "Message", "200", nil},
	{"post", "/api/storage/checkpoint", "Armazenamento", "Cria um checkpoint consistente", "operator", "", "Message", "200", nil},
	{"get", "/api/clients", "Monitoramento", "Conexões abertas no servidor", "reader", "", "ClientList", "200", nil},
	{"get", "/api/slowlog", "Monitoramento", "Comandos lentos", "reader", "", "SlowLog", "200", []string{"n"}},
	{"get", "/api/config", "Administração", "Parâmetros do servidor", "admin", "", "ConfigList", "200", nil},
	{"put", "/api/config", "Administração", "Altera um parâmetro em tempo de execução", "admin", "ConfigEntry", "", "204", nil},
	{"get", "/api/users", "Administração", "Lista usuários do painel", "admin", "", "UserList", "200", nil},
	{"post", "/api/users", "Administração", "Cria um usuário", "admin", "UserCreate", "User", "201", nil},
	{"patch", "/api/users/{email}", "Administração", "Altera nome, papel, senha ou desbloqueia", "admin", "UserPatch", "User", "200", nil},
	{"delete", "/api/users/{email}", "Administração", "Remove um usuário", "admin", "", "", "204", nil},
	{"get", "/api/audit", "Administração", "Registro de auditoria", "admin", "", "AuditList", "200", []string{"limit"}},
	{"get", "/api/health", "Monitoramento", "Saúde do gateway e do servidor", "", "", "Health", "200", nil},
	{"get", "/metrics", "Monitoramento", "Métricas no formato de exposição do Prometheus (text/plain 0.0.4)", "reader", "", "", "200", nil},
	{"get", "/api/integrations", "Integração", "Endereços e versão para configurar clientes e integrações", "reader", "", "Integrations", "200", nil},
	{"get", "/api/tokens", "Integração", "Lista as suas chaves de acesso (admin: all=1 lista de todos)", "reader", "", "TokenList", "200", []string{"all"}},
	{"post", "/api/tokens", "Integração", "Cria uma chave de acesso; o valor só aparece nesta resposta", "reader", "TokenCreate", "TokenCreated", "201", nil},
	{"delete", "/api/tokens/{id}", "Integração", "Revoga uma chave de acesso", "reader", "", "", "204", nil},
	{"get", "/api/webhooks/events", "Integração", "Eventos que podem disparar webhooks", "admin", "", "WebhookEventList", "200", nil},
	{"get", "/api/webhooks", "Integração", "Lista os webhooks", "admin", "", "WebhookList", "200", nil},
	{"post", "/api/webhooks", "Integração", "Cria um webhook; o segredo de assinatura só aparece nesta resposta", "admin", "WebhookCreate", "WebhookSaved", "201", nil},
	{"patch", "/api/webhooks/{id}", "Integração", "Altera endereço, eventos, estado ou gira o segredo", "admin", "WebhookPatch", "WebhookSaved", "200", nil},
	{"delete", "/api/webhooks/{id}", "Integração", "Remove um webhook e o histórico de entregas", "admin", "", "", "204", nil},
	{"post", "/api/webhooks/{id}/test", "Integração", "Envia um evento webhook.test e devolve o resultado", "admin", "", "WebhookTest", "200", nil},
	{"get", "/api/webhooks/{id}/deliveries", "Integração", "Últimas 30 entregas do webhook", "admin", "", "DeliveryList", "200", nil},
	{"get", "/api/keys/export", "Chaves", "Exporta chaves em JSON (anexo, até 100 000)", "reader", "", "KeyExport", "200", []string{"match"}},
	{"post", "/api/keys/import", "Chaves", "Importa chaves de um arquivo exportado (até 16 MB)", "operator", "KeyImport", "KeyImportResult", "200", nil},
	{"get", "/api/tables/{name}/export", "Tabelas", "Exporta todas as linhas em CSV ou JSON (anexo)", "reader", "", "", "200", []string{"format"}},
	{"post", "/api/tables/{name}/import", "Tabelas", "Importa linhas de um CSV com cabeçalho ou de uma lista de objetos", "operator", "TableImport", "TableImportResult", "200", nil},
	{"get", "/api/queries", "SQL", "Consultas salvas: as suas e as compartilhadas", "reader", "", "QueryList", "200", nil},
	{"post", "/api/queries", "SQL", "Salva uma consulta", "reader", "QueryCreate", "Query", "201", nil},
	{"delete", "/api/queries/{id}", "SQL", "Apaga uma consulta salva (dono ou admin)", "reader", "", "", "204", nil},
	{"get", "/api/storage/checkpoints", "Armazenamento", "Checkpoints guardados ao lado da pasta de dados", "reader", "", "CheckpointList", "200", nil},
	{"delete", "/api/storage/checkpoints/{name}", "Armazenamento", "Apaga um checkpoint", "admin", "", "", "204", nil},
	{"post", "/api/storage/verify", "Armazenamento", "Confere o checksum de todas as SSTables vivas", "operator", "", "VerifyResult", "200", nil},
}

func obj(props map[string]any, required ...string) map[string]any {
	o := map[string]any{"type": "object", "properties": props}
	if len(required) > 0 {
		o["required"] = required
	}
	return o
}

func str() map[string]any     { return map[string]any{"type": "string"} }
func integer() map[string]any { return map[string]any{"type": "integer"} }
func number() map[string]any  { return map[string]any{"type": "number"} }
func boolean() map[string]any { return map[string]any{"type": "boolean"} }
func arr(items any) map[string]any {
	return map[string]any{"type": "array", "items": items}
}
func ref(name string) map[string]any {
	return map[string]any{"$ref": "#/components/schemas/" + name}
}

func nullableOf(t map[string]any) map[string]any {
	return map[string]any{"anyOf": []any{t, map[string]any{"type": "null"}}}
}

func schemas() map[string]any {
	tokenInfo := obj(map[string]any{"id": str(), "name": str(), "role": str(), "prefix": str(), "owner": str(), "created_at": integer(), "last_used_at": nullableOf(integer()), "expires_at": nullableOf(integer())})
	user := obj(map[string]any{"email": str(), "name": str(), "role": map[string]any{"type": "string", "enum": []string{"reader", "operator", "admin"}}, "created_at": integer(), "locked": boolean()})
	return map[string]any{
		"Error":             obj(map[string]any{"code": str(), "message": str(), "fields": map[string]any{"type": "object", "additionalProperties": str()}}, "code", "message"),
		"LoginRequest":      obj(map[string]any{"email": str(), "password": str()}, "email", "password"),
		"LoginResponse":     obj(map[string]any{"token": str(), "user": ref("User"), "expires_at": integer()}),
		"User":              user,
		"UserList":          arr(ref("User")),
		"UserCreate":        obj(map[string]any{"email": str(), "name": str(), "role": str(), "password": str()}, "email", "name", "role", "password"),
		"UserPatch":         obj(map[string]any{"name": str(), "role": str(), "password": str(), "unlock": boolean()}),
		"PasswordChange":    obj(map[string]any{"current": str(), "new": str()}, "current", "new"),
		"Stats":             map[string]any{"type": "object", "description": "Saída de ARCA.STATS com o campo tables"},
		"KeyPage":           obj(map[string]any{"cursor": str(), "keys": arr(obj(map[string]any{"key": str(), "type": str(), "ttl_ms": integer(), "size": integer()}))}),
		"KeyValue":          obj(map[string]any{"key": str(), "value": str(), "ttl_ms": integer(), "size": integer(), "binary": boolean()}),
		"KeyPut":            obj(map[string]any{"key": str(), "value": str(), "ttl_s": integer()}, "key", "value"),
		"KeyExpire":         obj(map[string]any{"key": str(), "ttl_s": integer()}, "key"),
		"TableList":         arr(obj(map[string]any{"name": str(), "rows": integer(), "columns": arr(obj(map[string]any{"name": str(), "type": str(), "primary_key": boolean(), "not_null": boolean(), "default": map[string]any{}, "auto_increment": boolean()})), "indexes": arr(obj(map[string]any{"name": str(), "column": str()}))})),
		"RowPage":           obj(map[string]any{"columns": arr(str()), "types": arr(str()), "rows": arr(arr(map[string]any{})), "total": integer(), "primary_key": str()}),
		"RowWrite":          obj(map[string]any{"key": map[string]any{}, "values": map[string]any{"type": "object", "additionalProperties": map[string]any{}}}),
		"SQLRequest":        obj(map[string]any{"query": str()}, "query"),
		"SQLResult":         obj(map[string]any{"kind": map[string]any{"type": "string", "enum": []string{"rows", "command"}}, "columns": arr(str()), "types": arr(str()), "rows": arr(arr(map[string]any{})), "plan": str(), "tag": str(), "affected": integer(), "ms": number()}),
		"CommandRequest":    obj(map[string]any{"args": arr(str())}, "args"),
		"CommandResult":     obj(map[string]any{"reply": map[string]any{}, "ms": number()}),
		"Storage":           obj(map[string]any{"levels": arr(map[string]any{"type": "object"}), "engine": map[string]any{"type": "object"}, "recent_compactions": arr(map[string]any{"type": "object"})}),
		"Message":           obj(map[string]any{"message": str()}),
		"ClientList":        arr(obj(map[string]any{"id": integer(), "addr": str(), "name": str(), "age_s": integer(), "idle_s": integer(), "cmd": str()})),
		"SlowLog":           arr(obj(map[string]any{"id": integer(), "at": integer(), "micros": integer(), "args": arr(str()), "addr": str()})),
		"ConfigEntry":       obj(map[string]any{"name": str(), "value": str()}, "name", "value"),
		"ConfigList":        arr(ref("ConfigEntry")),
		"AuditList":         arr(obj(map[string]any{"id": integer(), "at": integer(), "email": str(), "action": str(), "detail": str()})),
		"Health":            obj(map[string]any{"status": str(), "arca": str(), "ready": boolean()}),
		"Integrations":      obj(map[string]any{"version": str(), "redis_host": str(), "redis_port": integer(), "http_base": str(), "openapi_url": str(), "metrics_url": str(), "requires_password": boolean()}),
		"TokenInfo":         tokenInfo,
		"TokenList":         arr(ref("TokenInfo")),
		"TokenCreate":       obj(map[string]any{"name": str(), "role": map[string]any{"type": "string", "enum": []string{"reader", "operator", "admin"}}, "expires_days": nullableOf(integer())}, "name", "role"),
		"TokenCreated":      obj(map[string]any{"token": str(), "info": ref("TokenInfo")}),
		"WebhookEventList":  arr(obj(map[string]any{"id": str(), "label": str()})),
		"Webhook":           obj(map[string]any{"id": str(), "url": str(), "events": arr(str()), "active": boolean(), "created_at": integer(), "last_status": nullableOf(integer()), "last_delivery_at": nullableOf(integer()), "failures": integer()}),
		"WebhookList":       arr(ref("Webhook")),
		"WebhookCreate":     obj(map[string]any{"url": str(), "events": arr(str()), "active": boolean()}, "url", "events"),
		"WebhookPatch":      obj(map[string]any{"url": str(), "events": arr(str()), "active": boolean(), "rotate_secret": boolean()}),
		"WebhookSaved":      obj(map[string]any{"webhook": ref("Webhook"), "secret": str()}),
		"WebhookTest":       obj(map[string]any{"status": integer(), "ms": number(), "error": str()}),
		"DeliveryList":      arr(obj(map[string]any{"id": str(), "at": integer(), "event": str(), "status": integer(), "ms": number(), "error": str()})),
		"KeyExport":         obj(map[string]any{"format": str(), "version": integer(), "exported_at": integer(), "match": str(), "count": integer(), "truncated": boolean(), "keys": arr(ref("ExportedKey"))}),
		"ExportedKey":       obj(map[string]any{"key": str(), "value": str(), "ttl_ms": nullableOf(integer()), "base64": boolean()}, "key", "value"),
		"KeyImport":         obj(map[string]any{"keys": arr(ref("ExportedKey")), "mode": map[string]any{"type": "string", "enum": []string{"overwrite", "skip"}}}, "keys"),
		"KeyImportResult":   obj(map[string]any{"imported": integer(), "skipped": integer(), "failed": integer()}),
		"TableImport":       obj(map[string]any{"csv": str(), "rows": arr(map[string]any{"type": "object", "additionalProperties": map[string]any{}})}),
		"TableImportResult": obj(map[string]any{"inserted": integer(), "failed": integer(), "errors": arr(obj(map[string]any{"line": integer(), "message": str()}))}),
		"Query":             obj(map[string]any{"id": str(), "name": str(), "query": str(), "owner": str(), "shared": boolean(), "created_at": integer()}),
		"QueryList":         arr(ref("Query")),
		"QueryCreate":       obj(map[string]any{"name": str(), "query": str(), "shared": boolean()}, "name", "query"),
		"CheckpointList":    arr(obj(map[string]any{"name": str(), "created_ms": integer(), "bytes": integer(), "files": integer()})),
		"VerifyResult":      obj(map[string]any{"files": integer(), "bytes": integer(), "ok": integer(), "corrupt": arr(obj(map[string]any{"file": str(), "error": str()})), "ms": number()}),
	}
}

var pathParam = regexp.MustCompile(`\{([a-z]+)\}`)

var (
	openapiOnce sync.Once
	openapiDoc  map[string]any
)

func buildOpenAPI() map[string]any {
	paths := map[string]any{}
	errResp := map[string]any{"description": "Erro", "content": map[string]any{"application/json": map[string]any{"schema": ref("Error")}}}
	for _, d := range routeDocs {
		op := map[string]any{"tags": []string{d.tag}, "summary": d.summary}
		desc := "Acesso público."
		if d.role != "" {
			desc = "Papel mínimo: " + d.role + "."
			op["security"] = []any{map[string]any{"bearer": []string{}}}
		}
		op["description"] = desc
		var params []any
		for _, m := range pathParam.FindAllStringSubmatch(d.path, -1) {
			params = append(params, map[string]any{"name": m[1], "in": "path", "required": true, "schema": str()})
		}
		for _, q := range d.query {
			params = append(params, map[string]any{"name": q, "in": "query", "schema": str()})
		}
		if len(params) > 0 {
			op["parameters"] = params
		}
		if d.request != "" {
			op["requestBody"] = map[string]any{"required": true, "content": map[string]any{"application/json": map[string]any{"schema": ref(d.request)}}}
		}
		ok := map[string]any{"description": "Sucesso"}
		if d.response != "" {
			ok["content"] = map[string]any{"application/json": map[string]any{"schema": ref(d.response)}}
		}
		switch d.path {
		case "/api/events":
			ok["content"] = map[string]any{"text/event-stream": map[string]any{"schema": str()}}
		case "/metrics":
			ok["content"] = map[string]any{"text/plain": map[string]any{"schema": str()}}
		case "/api/tables/{name}/export":
			ok["content"] = map[string]any{"text/csv": map[string]any{"schema": str()}, "application/json": map[string]any{"schema": arr(map[string]any{"type": "object"})}}
		}
		op["responses"] = map[string]any{d.status: ok, "400": errResp, "401": errResp, "403": errResp, "503": errResp}
		item, _ := paths[d.path].(map[string]any)
		if item == nil {
			item = map[string]any{}
			paths[d.path] = item
		}
		item[strings.ToLower(d.method)] = op
	}
	return map[string]any{
		"openapi": "3.1.0",
		"info": map[string]any{
			"title":       "Arca Gateway",
			"version":     Version,
			"description": "API HTTP do painel do Arca. Todas as rotas respondem JSON e, exceto login, saúde e esta especificação, exigem Authorization: Bearer <token>. O token pode ser o de uma sessão do painel ou uma chave de acesso (arca_...) criada em Integrações.",
			"license":     map[string]any{"name": "MIT"},
		},
		"servers": []any{map[string]any{"url": "/"}},
		"paths":   paths,
		"components": map[string]any{
			"schemas":         schemas(),
			"securitySchemes": map[string]any{"bearer": map[string]any{"type": "http", "scheme": "bearer"}},
		},
	}
}

func (s *Server) handleOpenAPI(w http.ResponseWriter, r *http.Request) {
	openapiOnce.Do(func() { openapiDoc = buildOpenAPI() })
	writeJSON(w, http.StatusOK, openapiDoc)
}

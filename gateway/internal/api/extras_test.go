package api

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestAPITokens(t *testing.T) {
	e := setup(t)
	op := e.login("operador@arca.dev")
	r := e.call("POST", "/api/tokens", op, map[string]any{"name": "", "role": "admin"})
	expect(t, r, 400)
	r = e.call("POST", "/api/tokens", op, map[string]any{"name": "CI", "role": "admin"})
	expect(t, r, 400)
	if !bytes.Contains(r.body, []byte("acima do seu papel")) {
		t.Fatalf("esperava recusa de papel acima: %s", r.body)
	}
	r = e.call("POST", "/api/tokens", op, map[string]any{"name": "Leitor do CI", "role": "reader", "expires_days": 30})
	expect(t, r, 201)
	var created struct {
		Token string    `json:"token"`
		Info  tokenView `json:"info"`
	}
	r.json(t, &created)
	if !strings.HasPrefix(created.Token, "arca_") || len(created.Token) != 45 || created.Info.ExpiresAt == nil || !strings.HasPrefix(created.Token, created.Info.Prefix) {
		t.Fatalf("token inesperado: %s", r.body)
	}
	for k := range e.fake.kv {
		if strings.Contains(k, created.Token) {
			t.Fatal("token salvo em claro")
		}
	}
	for _, tb := range e.fake.tables {
		for _, row := range tb.rows {
			for _, v := range row {
				if s, ok := v.(string); ok && strings.Contains(s, created.Token) {
					t.Fatal("token salvo em claro numa tabela")
				}
			}
		}
	}
	tok := created.Token
	r = e.call("GET", "/api/auth/me", tok, nil)
	expect(t, r, 200)
	if !bytes.Contains(r.body, []byte(`"reader"`)) {
		t.Fatalf("token deveria agir como leitura: %s", r.body)
	}
	expect(t, e.call("GET", "/api/keys", tok, nil), 200)
	expect(t, e.call("PUT", "/api/keys/value", tok, map[string]any{"key": "x", "value": "1"}), 403)
	expect(t, e.call("POST", "/api/tokens", tok, map[string]any{"name": "outro", "role": "reader"}), 403)
	r = e.call("GET", "/metrics", tok, nil)
	expect(t, r, 200)

	r = e.call("POST", "/api/tokens", op, map[string]any{"name": "Gravador", "role": "operator"})
	expect(t, r, 201)
	var writer struct {
		Token string    `json:"token"`
		Info  tokenView `json:"info"`
	}
	r.json(t, &writer)
	expect(t, e.call("PUT", "/api/keys/value", writer.Token, map[string]any{"key": "via:token", "value": "1"}), 204)
	admin := e.login("admin@arca.dev")
	r = e.call("GET", "/api/audit", admin, nil)
	if !bytes.Contains(r.body, []byte("operador@arca.dev (token Gravador)")) {
		t.Fatalf("auditoria sem o nome do token: %s", r.body)
	}

	r = e.call("GET", "/api/tokens", op, nil)
	var list []tokenView
	r.json(t, &list)
	if len(list) != 2 {
		t.Fatalf("esperava 2 tokens: %s", r.body)
	}
	if bytes.Contains(r.body, []byte(tok)) {
		t.Fatal("lista expõe o token")
	}
	r = e.call("GET", "/api/tokens?all=1", e.login("leitura@arca.dev"), nil)
	expect(t, r, 200)
	if strings.TrimSpace(string(r.body)) != "[]" {
		t.Fatalf("leitura não deveria ver tokens dos outros: %s", r.body)
	}
	r = e.call("GET", "/api/tokens?all=1", admin, nil)
	list = nil
	r.json(t, &list)
	if len(list) != 2 {
		t.Fatalf("admin com all=1 deveria ver 2: %s", r.body)
	}
	expect(t, e.call("DELETE", "/api/tokens/"+created.Info.ID, e.login("leitura@arca.dev"), nil), 404)
	expect(t, e.call("DELETE", "/api/tokens/"+created.Info.ID, op, nil), 204)
	expect(t, e.call("GET", "/api/auth/me", tok, nil), 401)
	expect(t, e.call("GET", "/api/auth/me", "arca_naoexiste", nil), 401)
}

func TestWebhooks(t *testing.T) {
	e := setup(t)
	admin := e.login("admin@arca.dev")
	var mu sync.Mutex
	type got struct {
		event, delivery, sig string
		body                 []byte
	}
	var received []got
	hits := make(chan struct{}, 16)
	failing := false
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		mu.Lock()
		received = append(received, got{r.Header.Get("X-Arca-Event"), r.Header.Get("X-Arca-Delivery"), r.Header.Get("X-Arca-Signature"), body})
		fail := failing
		mu.Unlock()
		if fail {
			w.WriteHeader(500)
		} else {
			w.WriteHeader(204)
		}
		hits <- struct{}{}
	}))
	defer target.Close()

	expect(t, e.call("GET", "/api/webhooks", e.login("operador@arca.dev"), nil), 403)
	r := e.call("GET", "/api/webhooks/events", admin, nil)
	expect(t, r, 200)
	if !bytes.Contains(r.body, []byte("key.write")) {
		t.Fatalf("eventos ausentes: %s", r.body)
	}
	r = e.call("POST", "/api/webhooks", admin, map[string]any{"url": "ftp://x", "events": []string{"nada"}})
	expect(t, r, 400)
	var apiErr apiError
	r.json(t, &apiErr)
	if apiErr.Fields["url"] == "" || apiErr.Fields["events"] == "" {
		t.Fatalf("campos esperados: %s", r.body)
	}
	r = e.call("POST", "/api/webhooks", admin, map[string]any{"url": target.URL + "/arca", "events": []string{"key.write", "key.delete"}})
	expect(t, r, 201)
	var saved struct {
		Webhook webhookView `json:"webhook"`
		Secret  string      `json:"secret"`
	}
	r.json(t, &saved)
	if !strings.HasPrefix(saved.Secret, "whsec_") || !saved.Webhook.Active || saved.Webhook.LastStatus != nil {
		t.Fatalf("webhook inesperado: %s", r.body)
	}
	r = e.call("GET", "/api/webhooks", admin, nil)
	if bytes.Contains(r.body, []byte(saved.Secret)) {
		t.Fatal("lista expõe o segredo")
	}

	expect(t, e.call("PUT", "/api/keys/value", admin, map[string]any{"key": "evento:1", "value": "a"}), 204)
	select {
	case <-hits:
	case <-time.After(5 * time.Second):
		t.Fatal("webhook não recebeu o evento")
	}
	e.srv.Hooks().Wait()
	mu.Lock()
	first := received[0]
	mu.Unlock()
	mac := hmac.New(sha256.New, []byte(saved.Secret))
	mac.Write(first.body)
	if first.sig != "sha256="+hex.EncodeToString(mac.Sum(nil)) {
		t.Fatalf("assinatura inválida: %s", first.sig)
	}
	var payload hookPayload
	if err := json.Unmarshal(first.body, &payload); err != nil || payload.Event != "key.write" || first.event != "key.write" || payload.Detail != "evento:1" || payload.Actor != "admin@arca.dev" || payload.ID != first.delivery {
		t.Fatalf("payload inesperado: %s", first.body)
	}

	// Evento que o webhook não assina não é entregue.
	expect(t, e.call("POST", "/api/storage/flush", admin, nil), 200)
	e.srv.Hooks().Wait()
	mu.Lock()
	if len(received) != 1 {
		t.Fatalf("entrega inesperada: %d", len(received))
	}
	mu.Unlock()

	r = e.call("GET", "/api/webhooks/"+saved.Webhook.ID+"/deliveries", admin, nil)
	expect(t, r, 200)
	var deliveries []map[string]any
	r.json(t, &deliveries)
	if len(deliveries) != 1 || deliveries[0]["status"].(float64) != 204 || deliveries[0]["event"] != "key.write" {
		t.Fatalf("entregas inesperadas: %s", r.body)
	}

	r = e.call("POST", "/api/webhooks/"+saved.Webhook.ID+"/test", admin, nil)
	expect(t, r, 200)
	<-hits
	var test map[string]any
	r.json(t, &test)
	if test["status"].(float64) != 204 {
		t.Fatalf("teste inesperado: %s", r.body)
	}

	mu.Lock()
	failing = true
	mu.Unlock()
	r = e.call("POST", "/api/webhooks/"+saved.Webhook.ID+"/test", admin, nil)
	<-hits
	<-hits
	r.json(t, &test)
	if test["status"].(float64) != 500 || test["error"] == nil {
		t.Fatalf("falha esperada: %s", r.body)
	}
	r = e.call("GET", "/api/webhooks", admin, nil)
	var hooks []webhookView
	r.json(t, &hooks)
	if len(hooks) != 1 || hooks[0].Failures != 1 || hooks[0].LastStatus == nil || *hooks[0].LastStatus != 500 {
		t.Fatalf("estado do webhook inesperado: %s", r.body)
	}

	r = e.call("PATCH", "/api/webhooks/"+saved.Webhook.ID, admin, map[string]any{"active": false, "rotate_secret": true})
	expect(t, r, 200)
	var patched struct {
		Webhook webhookView `json:"webhook"`
		Secret  string      `json:"secret"`
	}
	r.json(t, &patched)
	if patched.Webhook.Active || patched.Secret == "" || patched.Secret == saved.Secret {
		t.Fatalf("patch inesperado: %s", r.body)
	}
	expect(t, e.call("DELETE", "/api/webhooks/"+saved.Webhook.ID, admin, nil), 204)
	expect(t, e.call("DELETE", "/api/webhooks/"+saved.Webhook.ID, admin, nil), 404)
}

func TestPrometheusMetrics(t *testing.T) {
	e := setup(t)
	expect(t, e.call("GET", "/metrics", "", nil), 401)
	token := e.login("leitura@arca.dev")
	e.call("GET", "/api/overview", token, nil)
	r := e.call("GET", "/metrics", token, nil)
	expect(t, r, 200)
	if !strings.HasPrefix(r.header.Get("Content-Type"), "text/plain; version=0.0.4") {
		t.Fatalf("content-type inesperado: %s", r.header.Get("Content-Type"))
	}
	body := string(r.body)
	for _, want := range []string{"arca_up 1", "# TYPE arca_commands_processed_total counter", `arca_sst_files{level="0"} 2`, `arca_gateway_http_requests_total{code="200"}`, "arca_block_cache_hit_ratio", "arca_gateway_sse_clients 0"} {
		if !strings.Contains(body, want) {
			t.Fatalf("métrica %q ausente:\n%s", want, body)
		}
	}
}

func TestKeysExportImport(t *testing.T) {
	e := setup(t)
	op := e.login("operador@arca.dev")
	expect(t, e.call("PUT", "/api/keys/value", op, map[string]any{"key": "exp:a", "value": "um"}), 204)
	expect(t, e.call("PUT", "/api/keys/value", op, map[string]any{"key": "exp:b", "value": "dois", "ttl_s": 600}), 204)
	e.fake.mu.Lock()
	e.fake.kv["exp:bin"] = fakeEntry{value: "\xff\x00\x01"}
	e.fake.mu.Unlock()
	r := e.call("GET", "/api/keys/export?match=exp:*", e.login("leitura@arca.dev"), nil)
	expect(t, r, 200)
	if !strings.Contains(r.header.Get("Content-Disposition"), "arca-chaves-") {
		t.Fatalf("anexo ausente: %v", r.header)
	}
	var dump struct {
		Format string        `json:"format"`
		Count  int           `json:"count"`
		Keys   []exportedKey `json:"keys"`
	}
	r.json(t, &dump)
	if dump.Format != "arca.keys" || dump.Count != 3 || dump.Keys[0].Key != "exp:a" || dump.Keys[0].TTLMs != nil || dump.Keys[1].TTLMs == nil || !dump.Keys[2].Base64 {
		t.Fatalf("exportação inesperada: %s", r.body)
	}
	if bytes.Contains(r.body, []byte("_painel")) {
		t.Fatal("exportou chaves do painel")
	}

	e.fake.mu.Lock()
	delete(e.fake.kv, "exp:a")
	delete(e.fake.kv, "exp:bin")
	e.fake.kv["exp:b"] = fakeEntry{value: "local"}
	e.fake.mu.Unlock()
	keys := []map[string]any{}
	for _, k := range dump.Keys {
		keys = append(keys, map[string]any{"key": k.Key, "value": k.Value, "ttl_ms": k.TTLMs, "base64": k.Base64})
	}
	keys = append(keys, map[string]any{"key": "_painel:sessao:x", "value": "x"})
	expect(t, e.call("POST", "/api/keys/import", e.login("leitura@arca.dev"), map[string]any{"keys": keys}), 403)
	r = e.call("POST", "/api/keys/import", op, map[string]any{"keys": keys, "mode": "skip"})
	expect(t, r, 200)
	var res map[string]int
	r.json(t, &res)
	if res["imported"] != 2 || res["skipped"] != 1 || res["failed"] != 1 {
		t.Fatalf("importação inesperada: %s", r.body)
	}
	e.fake.mu.Lock()
	if e.fake.kv["exp:b"].value != "local" || e.fake.kv["exp:bin"].value != "\xff\x00\x01" || e.fake.kv["exp:a"].value != "um" {
		t.Fatalf("valores inesperados: %+v", e.fake.kv)
	}
	e.fake.mu.Unlock()
	r = e.call("POST", "/api/keys/import", op, map[string]any{"keys": keys[:2], "mode": "overwrite"})
	r.json(t, &res)
	if res["imported"] != 2 || e.fake.kv["exp:b"].value != "dois" || e.fake.kv["exp:b"].expire.IsZero() {
		t.Fatalf("sobrescrita inesperada: %s", r.body)
	}
	expect(t, e.call("POST", "/api/keys/import", op, map[string]any{"keys": keys, "mode": "outro"}), 400)
}

func TestTableExportImport(t *testing.T) {
	e := setup(t)
	op := e.login("operador@arca.dev")
	expect(t, e.call("POST", "/api/sql", op, map[string]string{"query": "CREATE TABLE notas (id INT PRIMARY KEY AUTOINCREMENT, titulo TEXT NOT NULL, nota REAL, lida BOOL)"}), 200)
	r := e.call("POST", "/api/tables/notas/import", op, map[string]any{"csv": "\ufeffid;titulo;nota;lida\n;Primeira;9,5;sim\n;\"Com \"\"aspas\"\"\";7;não\n;Errada;abc;sim\n;Curta\n"})
	expect(t, r, 200)
	var res struct {
		Inserted int           `json:"inserted"`
		Failed   int           `json:"failed"`
		Errors   []importError `json:"errors"`
	}
	r.json(t, &res)
	if res.Inserted != 2 || res.Failed != 2 || res.Errors[0].Line != 4 || !strings.Contains(res.Errors[0].Message, "nota") || res.Errors[1].Line != 5 {
		t.Fatalf("importação CSV inesperada: %s", r.body)
	}
	r = e.call("POST", "/api/tables/notas/import", op, map[string]any{"csv": "id,desconhecida\n1,x\n"})
	expect(t, r, 400)
	if !bytes.Contains(r.body, []byte("desconhecida")) {
		t.Fatalf("esperava coluna desconhecida: %s", r.body)
	}
	r = e.call("POST", "/api/tables/notas/import", op, map[string]any{"rows": []map[string]any{{"titulo": "Pela API", "nota": 8, "lida": true}}})
	r.json(t, &res)
	if res.Inserted != 1 || res.Failed != 0 {
		t.Fatalf("importação por rows inesperada: %s", r.body)
	}
	reader := e.login("leitura@arca.dev")
	expect(t, e.call("POST", "/api/tables/notas/import", reader, map[string]any{"rows": []map[string]any{{"titulo": "x"}}}), 403)
	r = e.call("GET", "/api/tables/notas/export?format=csv", reader, nil)
	expect(t, r, 200)
	if !strings.HasPrefix(r.header.Get("Content-Type"), "text/csv") || !strings.Contains(r.header.Get("Content-Disposition"), "notas.csv") {
		t.Fatalf("cabeçalhos do CSV inesperados: %v", r.header)
	}
	lines := strings.Split(strings.TrimSpace(string(r.body)), "\n")
	if len(lines) != 4 || lines[0] != "id,titulo,nota,lida" || lines[2] != `2,"Com ""aspas""",7,false` {
		t.Fatalf("CSV inesperado:\n%s", r.body)
	}
	r = e.call("GET", "/api/tables/notas/export?format=json", reader, nil)
	expect(t, r, 200)
	var rows []map[string]any
	r.json(t, &rows)
	if len(rows) != 3 || rows[0]["titulo"] != "Primeira" || rows[0]["nota"].(float64) != 9.5 {
		t.Fatalf("JSON inesperado: %s", r.body)
	}
	if !bytes.HasPrefix(r.body, []byte(`[{"id":1,"titulo":"Primeira"`)) {
		t.Fatalf("ordem das colunas não preservada: %s", r.body)
	}
	expect(t, e.call("GET", "/api/tables/notas/export?format=xml", reader, nil), 400)
	expect(t, e.call("GET", "/api/tables/_painel_usuarios/export", e.login("admin@arca.dev"), nil), 404)
}

func TestSavedQueries(t *testing.T) {
	e := setup(t)
	op := e.login("operador@arca.dev")
	reader := e.login("leitura@arca.dev")
	expect(t, e.call("POST", "/api/queries", op, map[string]any{"name": "", "query": ""}), 400)
	r := e.call("POST", "/api/queries", op, map[string]any{"name": "Pedidos caros", "query": "SELECT * FROM pedidos ORDER BY total DESC LIMIT 10"})
	expect(t, r, 201)
	var private map[string]any
	r.json(t, &private)
	r = e.call("POST", "/api/queries", op, map[string]any{"name": "Estoque", "query": "SELECT * FROM produtos", "shared": true})
	expect(t, r, 201)
	var shared map[string]any
	r.json(t, &shared)
	r = e.call("GET", "/api/queries", reader, nil)
	var list []map[string]any
	r.json(t, &list)
	if len(list) != 1 || list[0]["name"] != "Estoque" {
		t.Fatalf("leitura deveria ver só a compartilhada: %s", r.body)
	}
	r = e.call("GET", "/api/queries", op, nil)
	list = nil
	r.json(t, &list)
	if len(list) != 2 {
		t.Fatalf("dono deveria ver as duas: %s", r.body)
	}
	expect(t, e.call("DELETE", "/api/queries/"+shared["id"].(string), reader, nil), 404)
	expect(t, e.call("DELETE", "/api/queries/"+shared["id"].(string), e.login("admin@arca.dev"), nil), 204)
	expect(t, e.call("DELETE", "/api/queries/"+private["id"].(string), op, nil), 204)
	expect(t, e.call("DELETE", "/api/queries/"+private["id"].(string), op, nil), 404)
}

func TestCheckpointsVerifyIntegrations(t *testing.T) {
	e := setup(t)
	op := e.login("operador@arca.dev")
	admin := e.login("admin@arca.dev")
	r := e.call("GET", "/api/storage/checkpoints", op, nil)
	expect(t, r, 200)
	if strings.TrimSpace(string(r.body)) != "[]" {
		t.Fatalf("esperava lista vazia: %s", r.body)
	}
	expect(t, e.call("POST", "/api/storage/checkpoint", op, nil), 200)
	r = e.call("GET", "/api/storage/checkpoints", op, nil)
	var cps []map[string]any
	r.json(t, &cps)
	if len(cps) != 1 || !strings.HasPrefix(cps[0]["name"].(string), "painel-") {
		t.Fatalf("checkpoints inesperados: %s", r.body)
	}
	name := cps[0]["name"].(string)
	expect(t, e.call("DELETE", "/api/storage/checkpoints/"+name, op, nil), 403)
	expect(t, e.call("DELETE", "/api/storage/checkpoints/nome%20ruim", admin, nil), 400)
	expect(t, e.call("DELETE", "/api/storage/checkpoints/"+name, admin, nil), 204)
	expect(t, e.call("DELETE", "/api/storage/checkpoints/"+name, admin, nil), 404)

	expect(t, e.call("POST", "/api/storage/verify", e.login("leitura@arca.dev"), nil), 403)
	r = e.call("POST", "/api/storage/verify", op, nil)
	expect(t, r, 200)
	var v map[string]any
	r.json(t, &v)
	if v["files"].(float64) != 9 || v["ok"].(float64) != 9 {
		t.Fatalf("verificação inesperada: %s", r.body)
	}

	r = e.call("GET", "/api/integrations", op, nil)
	expect(t, r, 200)
	var info map[string]any
	r.json(t, &info)
	if info["version"] != Version || info["redis_port"].(float64) != 7379 || !strings.HasPrefix(info["http_base"].(string), "http://127.0.0.1:") || info["metrics_url"] != "/metrics" {
		t.Fatalf("integrações inesperadas: %s", r.body)
	}
	if bytes.Contains(r.body, []byte("segredo-de-teste")) {
		t.Fatal("senha do servidor exposta")
	}
	r = e.call("GET", "/api/openapi.json", "", nil)
	var doc map[string]any
	r.json(t, &doc)
	paths := doc["paths"].(map[string]any)
	for _, p := range []string{"/api/tokens", "/api/webhooks/{id}/deliveries", "/metrics", "/api/keys/export", "/api/tables/{name}/import", "/api/queries", "/api/storage/verify", "/api/integrations"} {
		if paths[p] == nil {
			t.Fatalf("rota %s ausente da OpenAPI", p)
		}
	}
	if doc["info"].(map[string]any)["version"] != "1.2.1" {
		t.Fatalf("versão da OpenAPI: %v", doc["info"])
	}
}

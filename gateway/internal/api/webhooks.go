package api

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"sync"
	"time"

	"arca/gateway/internal/store"
)

type webhookEvent struct {
	ID    string `json:"id"`
	Label string `json:"label"`
}

var webhookEvents = []webhookEvent{
	{"key.write", "Chave gravada, importada ou com TTL alterado"},
	{"key.delete", "Chave apagada"},
	{"table.write", "Linha inserida, alterada, apagada ou importada pelo painel"},
	{"sql.write", "Instrução SQL de escrita executada"},
	{"storage.flush", "Memtable gravada em disco"},
	{"storage.compact", "Compactação concluída"},
	{"storage.checkpoint", "Checkpoint criado ou removido"},
	{"config.change", "Parâmetro do servidor alterado"},
	{"user.change", "Usuário criado, alterado ou removido"},
	{"auth.login", "Entrada no painel"},
}

// auditEvents liga as ações gravadas na auditoria aos eventos de webhook.
var auditEvents = map[string]string{
	"chave_gravada":       "key.write",
	"ttl_definido":        "key.write",
	"ttl_removido":        "key.write",
	"chaves_importadas":   "key.write",
	"chave_removida":      "key.delete",
	"linha_inserida":      "table.write",
	"linha_alterada":      "table.write",
	"linha_removida":      "table.write",
	"linhas_importadas":   "table.write",
	"sql":                 "sql.write",
	"flush":               "storage.flush",
	"compactacao":         "storage.compact",
	"checkpoint":          "storage.checkpoint",
	"checkpoint_removido": "storage.checkpoint",
	"config":              "config.change",
	"usuario_criado":      "user.change",
	"usuario_alterado":    "user.change",
	"usuario_removido":    "user.change",
	"login":               "auth.login",
}

func validEvent(id string) bool {
	for _, e := range webhookEvents {
		if e.ID == id {
			return true
		}
	}
	return false
}

const (
	maxWebhooks      = 20
	keepDeliveries   = 30
	deliveryTimeout  = 5 * time.Second
	deliveryAttempts = 2
)

type hookPayload struct {
	ID     string `json:"id"`
	Event  string `json:"event"`
	At     int64  `json:"at"`
	Actor  string `json:"actor"`
	Detail string `json:"detail"`
}

// Dispatcher entrega os eventos aos webhooks em segundo plano.
type Dispatcher struct {
	store  *store.Store
	log    *slog.Logger
	client *http.Client
	queue  chan hookPayload
	sem    chan struct{}

	mu      sync.Mutex
	cache   []store.Webhook
	cacheAt time.Time
	pending sync.WaitGroup
}

func NewDispatcher(st *store.Store, logger *slog.Logger) *Dispatcher {
	d := &Dispatcher{
		store:  st,
		log:    logger,
		client: &http.Client{Timeout: deliveryTimeout},
		queue:  make(chan hookPayload, 512),
		sem:    make(chan struct{}, 4),
	}
	go d.run()
	return d
}

func newDeliveryID() string {
	b := make([]byte, 12)
	rand.Read(b)
	return "dlv_" + hex.EncodeToString(b)
}

// Emit coloca um evento na fila sem bloquear quem chamou.
func (d *Dispatcher) Emit(event, actor, detail string) {
	p := hookPayload{ID: newDeliveryID(), Event: event, At: time.Now().UnixMilli(), Actor: actor, Detail: detail}
	d.pending.Add(1)
	select {
	case d.queue <- p:
	default:
		d.pending.Done()
		d.log.Warn("fila de webhooks cheia, evento descartado", "evento", event)
	}
}

// Wait espera a fila esvaziar (usado nos testes).
func (d *Dispatcher) Wait() {
	d.pending.Wait()
}

func (d *Dispatcher) Invalidate() {
	d.mu.Lock()
	d.cache = nil
	d.cacheAt = time.Time{}
	d.mu.Unlock()
}

func (d *Dispatcher) active(ctx context.Context) []store.Webhook {
	d.mu.Lock()
	if d.cache != nil && time.Since(d.cacheAt) < 10*time.Second {
		out := d.cache
		d.mu.Unlock()
		return out
	}
	d.mu.Unlock()
	list, err := d.store.Webhooks(ctx)
	if err != nil {
		d.log.Warn("webhooks não carregados", "erro", err)
		return nil
	}
	active := make([]store.Webhook, 0, len(list))
	for _, w := range list {
		if w.Active {
			active = append(active, w)
		}
	}
	d.mu.Lock()
	d.cache = active
	d.cacheAt = time.Now()
	d.mu.Unlock()
	return active
}

func (d *Dispatcher) run() {
	for p := range d.queue {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		hooks := d.active(ctx)
		cancel()
		for _, h := range hooks {
			if !h.Wants(p.Event) {
				continue
			}
			d.pending.Add(1)
			d.sem <- struct{}{}
			go func(h store.Webhook) {
				defer func() { <-d.sem; d.pending.Done() }()
				d.deliver(context.Background(), h, p)
			}(h)
		}
		d.pending.Done()
	}
}

func sign(secret string, body []byte) string {
	mac := hmac.New(sha256.New, []byte(secret))
	mac.Write(body)
	return "sha256=" + hex.EncodeToString(mac.Sum(nil))
}

// deliver envia o evento (até duas tentativas) e registra o resultado.
func (d *Dispatcher) deliver(ctx context.Context, h store.Webhook, p hookPayload) store.Delivery {
	body, _ := json.Marshal(p)
	var status int
	var errText string
	start := time.Now()
	for attempt := 1; attempt <= deliveryAttempts; attempt++ {
		status, errText = d.post(ctx, h, p, body)
		if status >= 200 && status < 300 {
			break
		}
		if attempt < deliveryAttempts {
			select {
			case <-ctx.Done():
			case <-time.After(time.Second):
			}
		}
	}
	dl := store.Delivery{ID: p.ID, At: time.Now().UnixMilli(), Event: p.Event, Status: int64(status), Ms: float64(time.Since(start).Microseconds()) / 1000, Error: errText}
	sctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := d.store.AddDelivery(sctx, h.ID, dl, keepDeliveries); err != nil {
		d.log.Warn("entrega de webhook não registrada", "webhook", h.ID, "erro", err)
	}
	failures := int64(0)
	if !(status >= 200 && status < 300) {
		failures = 1
		if cur, err := d.store.Webhook(sctx, h.ID); err == nil {
			failures = cur.Failures + 1
		}
	}
	if err := d.store.WebhookResult(sctx, h.ID, int64(status), dl.At, failures); err != nil {
		d.log.Warn("resultado do webhook não registrado", "webhook", h.ID, "erro", err)
	}
	return dl
}

func (d *Dispatcher) post(ctx context.Context, h store.Webhook, p hookPayload, body []byte) (int, string) {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, h.URL, bytes.NewReader(body))
	if err != nil {
		return 0, "endereço inválido"
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", "Arca-Webhook/"+Version)
	req.Header.Set("X-Arca-Event", p.Event)
	req.Header.Set("X-Arca-Delivery", p.ID)
	req.Header.Set("X-Arca-Signature", sign(h.Secret, body))
	res, err := d.client.Do(req)
	if err != nil {
		var uerr *url.Error
		if errors.As(err, &uerr) && uerr.Timeout() {
			return 0, "sem resposta em 5 s"
		}
		return 0, "falha de conexão: " + err.Error()
	}
	io.Copy(io.Discard, io.LimitReader(res.Body, 64<<10))
	res.Body.Close()
	if res.StatusCode < 200 || res.StatusCode >= 300 {
		return res.StatusCode, fmt.Sprintf("o destino respondeu %d", res.StatusCode)
	}
	return res.StatusCode, ""
}

// ---------- rotas ----------

type webhookView struct {
	ID             string   `json:"id"`
	URL            string   `json:"url"`
	Events         []string `json:"events"`
	Active         bool     `json:"active"`
	CreatedAt      int64    `json:"created_at"`
	LastStatus     *int64   `json:"last_status"`
	LastDeliveryAt *int64   `json:"last_delivery_at"`
	Failures       int64    `json:"failures"`
}

func viewWebhook(h store.Webhook) webhookView {
	v := webhookView{ID: h.ID, URL: h.URL, Events: h.Events, Active: h.Active, CreatedAt: h.CreatedAt, LastDeliveryAt: nullable(h.LastDeliveryAt), Failures: h.Failures}
	if h.LastDeliveryAt > 0 {
		st := h.LastStatus
		v.LastStatus = &st
	}
	if v.Events == nil {
		v.Events = []string{}
	}
	return v
}

func newWebhookSecret() string {
	b := make([]byte, 24)
	rand.Read(b)
	return "whsec_" + hex.EncodeToString(b)
}

func validateWebhookURL(raw string) (string, string) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return "", "obrigatório"
	}
	if len(raw) > 2000 {
		return "", "no máximo 2000 caracteres"
	}
	u, err := url.Parse(raw)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		return "", "use um endereço http:// ou https:// completo"
	}
	return raw, ""
}

func validateEvents(events []string) ([]string, string) {
	if len(events) == 0 {
		return nil, "escolha ao menos um evento"
	}
	seen := map[string]bool{}
	out := []string{}
	for _, e := range events {
		e = strings.TrimSpace(e)
		if !validEvent(e) {
			return nil, "evento desconhecido: " + e
		}
		if !seen[e] {
			seen[e] = true
			out = append(out, e)
		}
	}
	sort.Strings(out)
	return out, ""
}

func (s *Server) handleWebhookEvents(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, webhookEvents)
}

func (s *Server) handleWebhooks(w http.ResponseWriter, r *http.Request) {
	list, err := s.store.Webhooks(r.Context())
	if err != nil {
		s.fail(w, r, err)
		return
	}
	sort.Slice(list, func(i, j int) bool { return list[i].CreatedAt < list[j].CreatedAt })
	out := make([]webhookView, 0, len(list))
	for _, h := range list {
		out = append(out, viewWebhook(h))
	}
	writeJSON(w, http.StatusOK, out)
}

type webhookWrite struct {
	URL          *string  `json:"url"`
	Events       []string `json:"events"`
	Active       *bool    `json:"active"`
	RotateSecret bool     `json:"rotate_secret"`
}

func (s *Server) handleWebhookCreate(w http.ResponseWriter, r *http.Request) {
	var req webhookWrite
	if !decodeBody(w, r, &req) {
		return
	}
	fields := map[string]string{}
	raw := ""
	if req.URL != nil {
		raw = *req.URL
	}
	u, msg := validateWebhookURL(raw)
	if msg != "" {
		fields["url"] = msg
	}
	events, msg := validateEvents(req.Events)
	if msg != "" {
		fields["events"] = msg
	}
	if len(fields) > 0 {
		writeError(w, http.StatusBadRequest, "validation", "Confira os campos destacados.", fields)
		return
	}
	ctx := r.Context()
	list, err := s.store.Webhooks(ctx)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if len(list) >= maxWebhooks {
		writeError(w, http.StatusConflict, "conflict", "O limite é de 20 webhooks. Remova algum antes de criar outro.", nil)
		return
	}
	h := store.Webhook{ID: store.NewID(), URL: u, Events: events, Active: req.Active == nil || *req.Active, Secret: newWebhookSecret(), CreatedAt: s.store.Now().UnixMilli()}
	if err := s.store.CreateWebhook(ctx, h); err != nil {
		s.fail(w, r, err)
		return
	}
	s.hooks.Invalidate()
	s.audit(r, "webhook_criado", h.URL)
	writeJSON(w, http.StatusCreated, map[string]any{"webhook": viewWebhook(h), "secret": h.Secret})
}

func (s *Server) webhookParam(w http.ResponseWriter, r *http.Request) (*store.Webhook, bool) {
	h, err := s.store.Webhook(r.Context(), r.PathValue("id"))
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeError(w, http.StatusNotFound, "not_found", "Webhook não encontrado.", nil)
			return nil, false
		}
		s.fail(w, r, err)
		return nil, false
	}
	return h, true
}

func (s *Server) handleWebhookUpdate(w http.ResponseWriter, r *http.Request) {
	var req webhookWrite
	if !decodeBody(w, r, &req) {
		return
	}
	h, ok := s.webhookParam(w, r)
	if !ok {
		return
	}
	fields := map[string]string{}
	if req.URL != nil {
		u, msg := validateWebhookURL(*req.URL)
		if msg != "" {
			fields["url"] = msg
		} else {
			h.URL = u
		}
	}
	if req.Events != nil {
		events, msg := validateEvents(req.Events)
		if msg != "" {
			fields["events"] = msg
		} else {
			h.Events = events
		}
	}
	if len(fields) > 0 {
		writeError(w, http.StatusBadRequest, "validation", "Confira os campos destacados.", fields)
		return
	}
	if req.Active != nil {
		h.Active = *req.Active
	}
	out := map[string]any{}
	if req.RotateSecret {
		h.Secret = newWebhookSecret()
		out["secret"] = h.Secret
	}
	if err := s.store.UpdateWebhook(r.Context(), *h); err != nil {
		s.fail(w, r, err)
		return
	}
	s.hooks.Invalidate()
	s.audit(r, "webhook_alterado", h.URL)
	out["webhook"] = viewWebhook(*h)
	writeJSON(w, http.StatusOK, out)
}

func (s *Server) handleWebhookDelete(w http.ResponseWriter, r *http.Request) {
	h, ok := s.webhookParam(w, r)
	if !ok {
		return
	}
	if err := s.store.DeleteWebhook(r.Context(), h.ID); err != nil {
		s.fail(w, r, err)
		return
	}
	s.hooks.Invalidate()
	s.audit(r, "webhook_removido", h.URL)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleWebhookTest(w http.ResponseWriter, r *http.Request) {
	h, ok := s.webhookParam(w, r)
	if !ok {
		return
	}
	actor := currentUser(r).Email
	p := hookPayload{ID: newDeliveryID(), Event: "webhook.test", At: time.Now().UnixMilli(), Actor: actor, Detail: "Teste enviado pelo painel do Arca."}
	dl := s.hooks.deliver(r.Context(), *h, p)
	out := map[string]any{"status": dl.Status, "ms": dl.Ms}
	if dl.Error != "" {
		out["error"] = dl.Error
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *Server) handleWebhookDeliveries(w http.ResponseWriter, r *http.Request) {
	h, ok := s.webhookParam(w, r)
	if !ok {
		return
	}
	list, err := s.store.Deliveries(r.Context(), h.ID, keepDeliveries)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, list)
}

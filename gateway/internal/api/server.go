package api

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"arca/gateway/internal/auth"
	"arca/gateway/internal/config"
	"arca/gateway/internal/resp"
	"arca/gateway/internal/store"
)

// Version é a versão do gateway, exibida no OpenAPI, em /api/integrations e nos webhooks.
const Version = "1.2.1"

type Server struct {
	cfg    config.Config
	store  *store.Store
	client *resp.Client
	log    *slog.Logger
	hub    *Hub
	hooks  *Dispatcher
	mux    *http.ServeMux
	ready  atomic.Bool

	httpMu     sync.Mutex
	httpCounts map[int]int64
}

type ctxKey int

const userKey ctxKey = 1

func New(cfg config.Config, client *resp.Client, st *store.Store, logger *slog.Logger) *Server {
	s := &Server{cfg: cfg, store: st, client: client, log: logger, mux: http.NewServeMux(), httpCounts: map[int]int64{}}
	s.hub = NewHub(st, logger, time.Second)
	s.hooks = NewDispatcher(st, logger)
	s.routes()
	return s
}

// Hooks devolve o despachante de webhooks.
func (s *Server) Hooks() *Dispatcher {
	return s.hooks
}

func (s *Server) SetReady(v bool) {
	s.ready.Store(v)
}

func (s *Server) Ready() bool {
	return s.ready.Load()
}

func (s *Server) Hub() *Hub {
	return s.hub
}

func (s *Server) Handler() http.Handler {
	return s.recoverer(s.securityHeaders(s.logRequests(s.mux)))
}

func (s *Server) routes() {
	m := s.mux
	m.HandleFunc("GET /api/health", s.handleHealth)
	m.HandleFunc("GET /api/openapi.json", s.handleOpenAPI)
	m.HandleFunc("POST /api/auth/login", s.needReady(s.handleLogin))
	m.HandleFunc("GET /api/events", s.needReady(s.handleEvents))

	m.Handle("POST /api/auth/logout", s.auth(auth.RoleReader, s.handleLogout))
	m.Handle("GET /api/auth/me", s.auth(auth.RoleReader, s.handleMe))
	m.Handle("PUT /api/auth/me/password", s.auth(auth.RoleReader, s.handleChangePassword))

	m.Handle("GET /api/overview", s.auth(auth.RoleReader, s.handleOverview))
	m.Handle("GET /api/keys", s.auth(auth.RoleReader, s.handleKeys))
	m.Handle("GET /api/keys/value", s.auth(auth.RoleReader, s.handleKeyGet))
	m.Handle("PUT /api/keys/value", s.auth(auth.RoleOperator, s.handleKeyPut))
	m.Handle("DELETE /api/keys/value", s.auth(auth.RoleOperator, s.handleKeyDelete))
	m.Handle("POST /api/keys/expire", s.auth(auth.RoleOperator, s.handleKeyExpire))

	m.Handle("GET /api/tables", s.auth(auth.RoleReader, s.handleTables))
	m.Handle("GET /api/tables/{name}/rows", s.auth(auth.RoleReader, s.handleRows))
	m.Handle("POST /api/tables/{name}/rows", s.auth(auth.RoleOperator, s.handleRowInsert))
	m.Handle("PUT /api/tables/{name}/rows", s.auth(auth.RoleOperator, s.handleRowUpdate))
	m.Handle("DELETE /api/tables/{name}/rows", s.auth(auth.RoleOperator, s.handleRowDelete))

	m.Handle("POST /api/sql", s.auth(auth.RoleReader, s.handleSQL))
	m.Handle("POST /api/command", s.auth(auth.RoleAdmin, s.handleCommand))

	m.Handle("GET /api/storage", s.auth(auth.RoleReader, s.handleStorage))
	m.Handle("POST /api/storage/flush", s.auth(auth.RoleOperator, s.handleFlush))
	m.Handle("POST /api/storage/compact", s.auth(auth.RoleOperator, s.handleCompact))
	m.Handle("POST /api/storage/checkpoint", s.auth(auth.RoleOperator, s.handleCheckpoint))

	m.Handle("GET /api/clients", s.auth(auth.RoleReader, s.handleClients))
	m.Handle("GET /api/slowlog", s.auth(auth.RoleReader, s.handleSlowlog))
	m.Handle("GET /api/config", s.auth(auth.RoleAdmin, s.handleConfigGet))
	m.Handle("PUT /api/config", s.auth(auth.RoleAdmin, s.handleConfigSet))

	m.Handle("GET /api/users", s.auth(auth.RoleAdmin, s.handleUsers))
	m.Handle("POST /api/users", s.auth(auth.RoleAdmin, s.handleUserCreate))
	m.Handle("PATCH /api/users/{email}", s.auth(auth.RoleAdmin, s.handleUserUpdate))
	m.Handle("DELETE /api/users/{email}", s.auth(auth.RoleAdmin, s.handleUserDelete))
	m.Handle("GET /api/audit", s.auth(auth.RoleAdmin, s.handleAudit))

	m.Handle("GET /api/tokens", s.auth(auth.RoleReader, s.handleTokens))
	m.Handle("POST /api/tokens", s.auth(auth.RoleReader, s.handleTokenCreate))
	m.Handle("DELETE /api/tokens/{id}", s.auth(auth.RoleReader, s.handleTokenDelete))

	m.Handle("GET /api/webhooks/events", s.auth(auth.RoleAdmin, s.handleWebhookEvents))
	m.Handle("GET /api/webhooks", s.auth(auth.RoleAdmin, s.handleWebhooks))
	m.Handle("POST /api/webhooks", s.auth(auth.RoleAdmin, s.handleWebhookCreate))
	m.Handle("PATCH /api/webhooks/{id}", s.auth(auth.RoleAdmin, s.handleWebhookUpdate))
	m.Handle("DELETE /api/webhooks/{id}", s.auth(auth.RoleAdmin, s.handleWebhookDelete))
	m.Handle("POST /api/webhooks/{id}/test", s.auth(auth.RoleAdmin, s.handleWebhookTest))
	m.Handle("GET /api/webhooks/{id}/deliveries", s.auth(auth.RoleAdmin, s.handleWebhookDeliveries))

	m.Handle("GET /api/keys/export", s.auth(auth.RoleReader, s.handleKeysExport))
	m.Handle("POST /api/keys/import", s.auth(auth.RoleOperator, s.handleKeysImport))
	m.Handle("GET /api/tables/{name}/export", s.auth(auth.RoleReader, s.handleTableExport))
	m.Handle("POST /api/tables/{name}/import", s.auth(auth.RoleOperator, s.handleTableImport))

	m.Handle("GET /api/queries", s.auth(auth.RoleReader, s.handleQueries))
	m.Handle("POST /api/queries", s.auth(auth.RoleReader, s.handleQueryCreate))
	m.Handle("DELETE /api/queries/{id}", s.auth(auth.RoleReader, s.handleQueryDelete))

	m.Handle("GET /api/storage/checkpoints", s.auth(auth.RoleReader, s.handleCheckpoints))
	m.Handle("DELETE /api/storage/checkpoints/{name}", s.auth(auth.RoleAdmin, s.handleCheckpointDelete))
	m.Handle("POST /api/storage/verify", s.auth(auth.RoleOperator, s.handleVerify))

	m.Handle("GET /api/integrations", s.auth(auth.RoleReader, s.handleIntegrations))
	m.Handle("GET /metrics", s.auth(auth.RoleReader, s.handleMetrics))

	m.HandleFunc("/api/", func(w http.ResponseWriter, r *http.Request) {
		writeError(w, http.StatusNotFound, "not_found", "Rota não encontrada.", nil)
	})
	m.Handle("/", s.static())
}

type apiError struct {
	Code    string            `json:"code"`
	Message string            `json:"message"`
	Fields  map[string]string `json:"fields,omitempty"`
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		http.Error(w, `{"code":"internal","message":"Falha ao montar a resposta."}`, http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	w.Write(buf.Bytes())
}

func writeRaw(w http.ResponseWriter, status int, raw []byte) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	w.Write(raw)
}

func writeError(w http.ResponseWriter, status int, code, message string, fields map[string]string) {
	writeJSON(w, status, apiError{Code: code, Message: message, Fields: fields})
}

func (s *Server) fail(w http.ResponseWriter, r *http.Request, err error) {
	var unavailable *resp.UnavailableError
	var serverErr *resp.ServerError
	switch {
	case errors.Is(err, store.ErrNotFound):
		writeError(w, http.StatusNotFound, "not_found", "Registro não encontrado.", nil)
	case errors.As(err, &unavailable):
		writeError(w, http.StatusServiceUnavailable, "arca_unavailable", "O Arca não está respondendo. Confira se o servidor está no ar.", nil)
	case errors.As(err, &serverErr):
		writeError(w, http.StatusBadRequest, "arca_error", serverErr.Text(), nil)
	case errors.Is(err, context.Canceled):
		return
	default:
		s.log.Error("falha na requisição", "path", r.URL.Path, "erro", err)
		writeError(w, http.StatusInternalServerError, "internal", "Erro interno no gateway.", nil)
	}
}

func decodeBody(w http.ResponseWriter, r *http.Request, out any) bool {
	return decodeBodyLimit(w, r, out, 1<<20)
}

func decodeBodyLimit(w http.ResponseWriter, r *http.Request, out any, limit int64) bool {
	body := http.MaxBytesReader(w, r.Body, limit)
	data, err := io.ReadAll(body)
	if err != nil {
		writeError(w, http.StatusRequestEntityTooLarge, "too_large", fmt.Sprintf("O corpo da requisição passa de %d MB.", limit>>20), nil)
		return false
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	if err := dec.Decode(out); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_json", "JSON inválido no corpo da requisição.", nil)
		return false
	}
	return true
}

func currentUser(r *http.Request) *store.User {
	u, _ := r.Context().Value(userKey).(*store.User)
	return u
}

func bearerToken(r *http.Request) string {
	h := r.Header.Get("Authorization")
	if len(h) > 7 && strings.EqualFold(h[:7], "bearer ") {
		return strings.TrimSpace(h[7:])
	}
	return ""
}

func (s *Server) needReady(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !s.Ready() {
			writeError(w, http.StatusServiceUnavailable, "arca_unavailable", "O gateway ainda está aguardando o Arca responder.", nil)
			return
		}
		next(w, r)
	}
}

func (s *Server) authenticate(r *http.Request, token string) (*store.User, int, string) {
	if token == "" {
		return nil, http.StatusUnauthorized, "Faça login para continuar."
	}
	if strings.HasPrefix(token, apiTokenPrefix) {
		return s.authenticateToken(r, token)
	}
	email, err := s.store.SessionEmail(r.Context(), token)
	if err != nil {
		var unavailable *resp.UnavailableError
		if errors.As(err, &unavailable) {
			return nil, http.StatusServiceUnavailable, "O Arca não está respondendo."
		}
		return nil, http.StatusUnauthorized, "Sessão expirada. Entre novamente."
	}
	u, err := s.store.User(r.Context(), email)
	if err != nil {
		return nil, http.StatusUnauthorized, "Sessão expirada. Entre novamente."
	}
	return u, 0, ""
}

func (s *Server) auth(role string, next http.HandlerFunc) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !s.Ready() {
			writeError(w, http.StatusServiceUnavailable, "arca_unavailable", "O gateway ainda está aguardando o Arca responder.", nil)
			return
		}
		u, status, msg := s.authenticate(r, bearerToken(r))
		if u == nil {
			code := "unauthorized"
			if status == http.StatusServiceUnavailable {
				code = "arca_unavailable"
			}
			writeError(w, status, code, msg, nil)
			return
		}
		if !auth.Allows(u.Role, role) {
			writeError(w, http.StatusForbidden, "forbidden", "Seu papel não permite esta ação.", nil)
			return
		}
		next(w, r.WithContext(context.WithValue(r.Context(), userKey, u)))
	})
}

func (s *Server) audit(r *http.Request, action, detail string) {
	email := ""
	if u := currentUser(r); u != nil {
		email = u.Email
		if u.TokenName != "" {
			email += " (token " + u.TokenName + ")"
		}
	}
	ctx, cancel := context.WithTimeout(context.WithoutCancel(r.Context()), 5*time.Second)
	defer cancel()
	if err := s.store.Audit(ctx, email, action, detail); err != nil {
		s.log.Warn("auditoria não registrada", "acao", action, "erro", err)
	}
	if event, ok := auditEvents[action]; ok {
		s.hooks.Emit(event, email, detail)
	}
}

func (s *Server) countHTTP(status int) {
	s.httpMu.Lock()
	s.httpCounts[status]++
	s.httpMu.Unlock()
}

func (s *Server) httpSnapshot() map[int]int64 {
	s.httpMu.Lock()
	defer s.httpMu.Unlock()
	out := make(map[int]int64, len(s.httpCounts))
	for k, v := range s.httpCounts {
		out[k] = v
	}
	return out
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (r *statusRecorder) WriteHeader(code int) {
	r.status = code
	r.ResponseWriter.WriteHeader(code)
}

func (r *statusRecorder) Flush() {
	if f, ok := r.ResponseWriter.(http.Flusher); ok {
		f.Flush()
	}
}

func (r *statusRecorder) Unwrap() http.ResponseWriter {
	return r.ResponseWriter
}

func (s *Server) logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.URL.Path, "/api/") || r.URL.Path == "/api/events" {
			next.ServeHTTP(w, r)
			return
		}
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rec, r)
		s.countHTTP(rec.status)
		s.log.Info("http", "metodo", r.Method, "rota", r.URL.Path, "status", rec.status, "ms", float64(time.Since(start).Microseconds())/1000)
	})
}

func (s *Server) securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h := w.Header()
		h.Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self'; connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'")
		h.Set("X-Frame-Options", "DENY")
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("Referrer-Policy", "no-referrer")
		h.Set("Cross-Origin-Opener-Policy", "same-origin")
		if strings.HasPrefix(r.URL.Path, "/api/") {
			h.Set("Cache-Control", "no-store")
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) recoverer(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if v := recover(); v != nil {
				if v == http.ErrAbortHandler {
					panic(v)
				}
				s.log.Error("pânico na requisição", "path", r.URL.Path, "valor", v)
				writeError(w, http.StatusInternalServerError, "internal", "Erro interno no gateway.", nil)
			}
		}()
		next.ServeHTTP(w, r)
	})
}

func (s *Server) static() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			writeError(w, http.StatusMethodNotAllowed, "method_not_allowed", "Método não permitido.", nil)
			return
		}
		root := s.cfg.WebDir
		index := filepath.Join(root, "index.html")
		if _, err := os.Stat(index); err != nil {
			w.Header().Set("Content-Type", "text/plain; charset=utf-8")
			w.WriteHeader(http.StatusNotFound)
			io.WriteString(w, "Painel não encontrado. Rode npm run build na pasta web ou ajuste ARCA_WEB_DIR.\n")
			return
		}
		clean := path.Clean("/" + r.URL.Path)
		file := filepath.Join(root, filepath.FromSlash(clean))
		if info, err := os.Stat(file); err == nil && !info.IsDir() {
			if strings.HasPrefix(clean, "/assets/") {
				w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
			}
			http.ServeFile(w, r, file)
			return
		}
		if path.Ext(clean) != "" && clean != "/" {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Cache-Control", "no-cache")
		http.ServeFile(w, r, index)
	})
}

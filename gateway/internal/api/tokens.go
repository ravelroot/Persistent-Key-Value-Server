package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"arca/gateway/internal/auth"
	"arca/gateway/internal/resp"
	"arca/gateway/internal/store"
)

const (
	apiTokenPrefix   = "arca_"
	maxTokensPerUser = 50
	tokenTouchEvery  = time.Minute
)

var roleOrder = map[string]int{auth.RoleReader: 1, auth.RoleOperator: 2, auth.RoleAdmin: 3}

func minRole(a, b string) string {
	if roleOrder[a] <= roleOrder[b] {
		return a
	}
	return b
}

func newAPIToken() (string, error) {
	b := make([]byte, 20)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return apiTokenPrefix + hex.EncodeToString(b), nil
}

func (s *Server) authenticateToken(r *http.Request, token string) (*store.User, int, string) {
	ctx := r.Context()
	tk, err := s.store.TokenByHash(ctx, auth.TokenHash(token))
	if err != nil {
		var unavailable *resp.UnavailableError
		if errors.As(err, &unavailable) {
			return nil, http.StatusServiceUnavailable, "O Arca não está respondendo."
		}
		return nil, http.StatusUnauthorized, "Chave de acesso inválida ou revogada."
	}
	now := s.store.Now()
	if tk.ExpiresAt > 0 && now.UnixMilli() >= tk.ExpiresAt {
		return nil, http.StatusUnauthorized, "Chave de acesso expirada. Crie uma nova em Integrações."
	}
	u, err := s.store.User(ctx, tk.Owner)
	if err != nil {
		return nil, http.StatusUnauthorized, "O dono desta chave de acesso não existe mais."
	}
	eff := *u
	eff.Role = minRole(u.Role, tk.Role)
	eff.TokenName = tk.Name
	if now.UnixMilli()-tk.LastUsedAt > tokenTouchEvery.Milliseconds() {
		tctx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 2*time.Second)
		if err := s.store.TouchToken(tctx, tk.ID, now.UnixMilli()); err != nil {
			s.log.Debug("último uso do token não registrado", "erro", err)
		}
		cancel()
	}
	return &eff, 0, ""
}

type tokenView struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Role       string `json:"role"`
	Prefix     string `json:"prefix"`
	Owner      string `json:"owner"`
	CreatedAt  int64  `json:"created_at"`
	LastUsedAt *int64 `json:"last_used_at"`
	ExpiresAt  *int64 `json:"expires_at"`
}

func nullable(v int64) *int64 {
	if v <= 0 {
		return nil
	}
	return &v
}

func viewToken(t store.APIToken) tokenView {
	return tokenView{ID: t.ID, Name: t.Name, Role: t.Role, Prefix: t.Prefix, Owner: t.Owner, CreatedAt: t.CreatedAt, LastUsedAt: nullable(t.LastUsedAt), ExpiresAt: nullable(t.ExpiresAt)}
}

func (s *Server) handleTokens(w http.ResponseWriter, r *http.Request) {
	u := currentUser(r)
	owner := u.Email
	if u.Role == auth.RoleAdmin && r.URL.Query().Get("all") == "1" {
		owner = ""
	}
	list, err := s.store.Tokens(r.Context(), owner)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	sort.Slice(list, func(i, j int) bool { return list[i].CreatedAt > list[j].CreatedAt })
	out := make([]tokenView, 0, len(list))
	for _, t := range list {
		out = append(out, viewToken(t))
	}
	writeJSON(w, http.StatusOK, out)
}

type tokenCreate struct {
	Name        string       `json:"name"`
	Role        string       `json:"role"`
	ExpiresDays *json.Number `json:"expires_days"`
}

func (s *Server) handleTokenCreate(w http.ResponseWriter, r *http.Request) {
	u := currentUser(r)
	if u.TokenName != "" {
		writeError(w, http.StatusForbidden, "forbidden", "Chaves de acesso só podem ser criadas numa sessão do painel.", nil)
		return
	}
	var req tokenCreate
	if !decodeBody(w, r, &req) {
		return
	}
	fields := map[string]string{}
	name := strings.TrimSpace(req.Name)
	if name == "" {
		fields["name"] = "obrigatório"
	} else if utf8.RuneCountInString(name) > 80 {
		fields["name"] = "no máximo 80 caracteres"
	}
	role := strings.TrimSpace(req.Role)
	if role == "" {
		role = auth.RoleReader
	}
	if !auth.ValidRole(role) {
		fields["role"] = "use reader, operator ou admin"
	} else if !auth.Allows(u.Role, role) {
		fields["role"] = "acima do seu papel"
	}
	var days int64
	if req.ExpiresDays != nil && req.ExpiresDays.String() != "" {
		v, err := strconv.ParseInt(req.ExpiresDays.String(), 10, 64)
		if err != nil || v < 0 || v > 3650 {
			fields["expires_days"] = "entre 1 e 3650 dias, ou vazio para não expirar"
		} else {
			days = v
		}
	}
	if len(fields) > 0 {
		msg := "Confira os campos destacados."
		if fields["role"] == "acima do seu papel" && len(fields) == 1 {
			msg = "Uma chave de acesso não pode ter papel acima do seu."
		}
		writeError(w, http.StatusBadRequest, "validation", msg, fields)
		return
	}
	ctx := r.Context()
	mine, err := s.store.Tokens(ctx, u.Email)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if len(mine) >= maxTokensPerUser {
		writeError(w, http.StatusConflict, "conflict", "Você já tem 50 chaves de acesso. Revogue alguma antes de criar outra.", nil)
		return
	}
	token, err := newAPIToken()
	if err != nil {
		s.fail(w, r, err)
		return
	}
	now := s.store.Now()
	t := store.APIToken{ID: store.NewID(), Hash: auth.TokenHash(token), Name: name, Role: role, Owner: u.Email, Prefix: token[:13], CreatedAt: now.UnixMilli()}
	if days > 0 {
		t.ExpiresAt = now.Add(time.Duration(days) * 24 * time.Hour).UnixMilli()
	}
	if err := s.store.CreateToken(ctx, t); err != nil {
		s.fail(w, r, err)
		return
	}
	s.audit(r, "token_criado", name+" ("+role+")")
	writeJSON(w, http.StatusCreated, map[string]any{"token": token, "info": viewToken(t)})
}

func (s *Server) handleTokenDelete(w http.ResponseWriter, r *http.Request) {
	u := currentUser(r)
	ctx := r.Context()
	t, err := s.store.Token(ctx, r.PathValue("id"))
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeError(w, http.StatusNotFound, "not_found", "Chave de acesso não encontrada.", nil)
			return
		}
		s.fail(w, r, err)
		return
	}
	if t.Owner != u.Email && u.Role != auth.RoleAdmin {
		writeError(w, http.StatusNotFound, "not_found", "Chave de acesso não encontrada.", nil)
		return
	}
	if err := s.store.DeleteToken(ctx, t.ID); err != nil {
		s.fail(w, r, err)
		return
	}
	s.audit(r, "token_revogado", t.Name+" de "+t.Owner)
	w.WriteHeader(http.StatusNoContent)
}

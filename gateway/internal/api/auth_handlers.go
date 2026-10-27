package api

import (
	"errors"
	"net/http"
	"strconv"
	"strings"
	"time"

	"arca/gateway/internal/auth"
	"arca/gateway/internal/store"
)

const (
	maxFailedLogins = 5
	lockDuration    = 5 * time.Minute
)

type loginRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
}

type userView struct {
	Email     string `json:"email"`
	Name      string `json:"name"`
	Role      string `json:"role"`
	CreatedAt int64  `json:"created_at,omitempty"`
	Locked    bool   `json:"locked"`
}

func viewOf(u *store.User, now time.Time) userView {
	return userView{Email: u.Email, Name: u.Name, Role: u.Role, CreatedAt: u.CreatedAt, Locked: u.Locked(now)}
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	var req loginRequest
	if !decodeBody(w, r, &req) {
		return
	}
	email, err := auth.NormalizeEmail(req.Email)
	if err != nil || req.Password == "" {
		writeError(w, http.StatusBadRequest, "validation", "Informe e-mail e senha.", map[string]string{"email": "obrigatório", "password": "obrigatório"})
		return
	}
	ctx := r.Context()
	now := s.store.Now()
	u, err := s.store.User(ctx, email)
	if errors.Is(err, store.ErrNotFound) {
		auth.BurnTime(req.Password)
		writeError(w, http.StatusUnauthorized, "invalid_credentials", "E-mail ou senha incorretos.", nil)
		return
	}
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if u.Locked(now) {
		wait := time.Until(time.UnixMilli(u.LockedUntil)).Round(time.Second)
		if wait < time.Second {
			wait = time.Second
		}
		writeError(w, http.StatusLocked, "locked", "Conta bloqueada por tentativas erradas. Tente de novo em "+formatWait(wait)+".", nil)
		return
	}
	if !auth.VerifyPassword(req.Password, u.Password) {
		failed := u.Failed + 1
		set := map[string]string{"failed": strconv.FormatInt(failed, 10)}
		msg := "E-mail ou senha incorretos."
		status := http.StatusUnauthorized
		code := "invalid_credentials"
		if failed >= maxFailedLogins {
			set["failed"] = "0"
			set["locked_until"] = strconv.FormatInt(now.Add(lockDuration).UnixMilli(), 10)
			msg = "Conta bloqueada por 5 minutos depois de 5 senhas erradas."
			status = http.StatusLocked
			code = "locked"
		}
		if err := s.store.UpdateUser(ctx, email, set); err != nil {
			s.fail(w, r, err)
			return
		}
		r = r.WithContext(withUser(ctx, &store.User{Email: email}))
		s.audit(r, "login_falhou", "tentativa "+strconv.FormatInt(failed, 10))
		writeError(w, status, code, msg, nil)
		return
	}
	if u.Failed != 0 || u.LockedUntil != 0 {
		if err := s.store.UpdateUser(ctx, email, map[string]string{"failed": "0", "locked_until": "0"}); err != nil {
			s.fail(w, r, err)
			return
		}
	}
	token, err := auth.NewToken()
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if err := s.store.CreateSession(ctx, token, email, s.cfg.SessionTTL); err != nil {
		s.fail(w, r, err)
		return
	}
	r = r.WithContext(withUser(ctx, u))
	s.audit(r, "login", "")
	writeJSON(w, http.StatusOK, map[string]any{
		"token":      token,
		"user":       viewOf(u, now),
		"expires_at": now.Add(s.cfg.SessionTTL).UnixMilli(),
	})
}

func formatWait(d time.Duration) string {
	if d >= time.Minute {
		m := int(d.Round(time.Minute) / time.Minute)
		if m == 1 {
			return "1 minuto"
		}
		return strconv.Itoa(m) + " minutos"
	}
	sec := int(d / time.Second)
	if sec == 1 {
		return "1 segundo"
	}
	return strconv.Itoa(sec) + " segundos"
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	if err := s.store.DeleteSession(r.Context(), bearerToken(r)); err != nil {
		s.fail(w, r, err)
		return
	}
	s.audit(r, "logout", "")
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleMe(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, viewOf(currentUser(r), s.store.Now()))
}

type passwordChange struct {
	Current string `json:"current"`
	New     string `json:"new"`
}

func (s *Server) handleChangePassword(w http.ResponseWriter, r *http.Request) {
	var req passwordChange
	if !decodeBody(w, r, &req) {
		return
	}
	u := currentUser(r)
	if !auth.VerifyPassword(req.Current, u.Password) {
		writeError(w, http.StatusBadRequest, "validation", "A senha atual está incorreta.", map[string]string{"current": "senha incorreta"})
		return
	}
	if err := auth.CheckPolicy(req.New); err != nil {
		writeError(w, http.StatusBadRequest, "validation", err.Error(), map[string]string{"new": err.Error()})
		return
	}
	if err := s.store.SetPassword(r.Context(), u.Email, req.New); err != nil {
		s.fail(w, r, err)
		return
	}
	s.audit(r, "senha_alterada", "")
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleUsers(w http.ResponseWriter, r *http.Request) {
	users, err := s.store.Users(r.Context())
	if err != nil {
		s.fail(w, r, err)
		return
	}
	now := s.store.Now()
	out := make([]userView, 0, len(users))
	for i := range users {
		out = append(out, viewOf(&users[i], now))
	}
	writeJSON(w, http.StatusOK, out)
}

type userCreate struct {
	Email    string `json:"email"`
	Name     string `json:"name"`
	Role     string `json:"role"`
	Password string `json:"password"`
}

func (s *Server) handleUserCreate(w http.ResponseWriter, r *http.Request) {
	var req userCreate
	if !decodeBody(w, r, &req) {
		return
	}
	fields := map[string]string{}
	email, err := auth.NormalizeEmail(req.Email)
	if err != nil {
		fields["email"] = err.Error()
	}
	name := strings.TrimSpace(req.Name)
	if name == "" || len(name) > 120 {
		fields["name"] = "Informe um nome com até 120 caracteres."
	}
	if !auth.ValidRole(req.Role) {
		fields["role"] = "Papel inválido. Use reader, operator ou admin."
	}
	if err := auth.CheckPolicy(req.Password); err != nil {
		fields["password"] = err.Error()
	}
	if len(fields) > 0 {
		writeError(w, http.StatusBadRequest, "validation", "Confira os campos destacados.", fields)
		return
	}
	if _, err := s.store.User(r.Context(), email); err == nil {
		writeError(w, http.StatusConflict, "conflict", "Já existe um usuário com este e-mail.", map[string]string{"email": "já cadastrado"})
		return
	} else if !errors.Is(err, store.ErrNotFound) {
		s.fail(w, r, err)
		return
	}
	if err := s.store.CreateUser(r.Context(), email, name, req.Role, req.Password); err != nil {
		s.fail(w, r, err)
		return
	}
	s.audit(r, "usuario_criado", email+" ("+req.Role+")")
	u, err := s.store.User(r.Context(), email)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	writeJSON(w, http.StatusCreated, viewOf(u, s.store.Now()))
}

type userPatch struct {
	Name     *string `json:"name"`
	Role     *string `json:"role"`
	Password *string `json:"password"`
	Unlock   bool    `json:"unlock"`
}

func (s *Server) handleUserUpdate(w http.ResponseWriter, r *http.Request) {
	email := strings.ToLower(strings.TrimSpace(r.PathValue("email")))
	var req userPatch
	if !decodeBody(w, r, &req) {
		return
	}
	ctx := r.Context()
	target, err := s.store.User(ctx, email)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	set := map[string]string{}
	fields := map[string]string{}
	var changes []string
	if req.Name != nil {
		name := strings.TrimSpace(*req.Name)
		if name == "" || len(name) > 120 {
			fields["name"] = "Informe um nome com até 120 caracteres."
		} else {
			set["name"] = store.Quote(name)
			changes = append(changes, "nome")
		}
	}
	if req.Role != nil && *req.Role != target.Role {
		if !auth.ValidRole(*req.Role) {
			fields["role"] = "Papel inválido. Use reader, operator ou admin."
		} else {
			if target.Role == auth.RoleAdmin {
				admins, err := s.store.CountAdmins(ctx)
				if err != nil {
					s.fail(w, r, err)
					return
				}
				if admins <= 1 {
					writeError(w, http.StatusConflict, "conflict", "É preciso manter pelo menos um administrador.", map[string]string{"role": "último administrador"})
					return
				}
			}
			set["role"] = store.Quote(*req.Role)
			changes = append(changes, "papel "+*req.Role)
		}
	}
	if req.Password != nil {
		if err := auth.CheckPolicy(*req.Password); err != nil {
			fields["password"] = err.Error()
		} else {
			hash, err := auth.HashPassword(*req.Password)
			if err != nil {
				s.fail(w, r, err)
				return
			}
			set["password"] = store.Quote(hash)
			set["failed"] = "0"
			set["locked_until"] = "0"
			changes = append(changes, "senha")
		}
	}
	if req.Unlock {
		set["failed"] = "0"
		set["locked_until"] = "0"
		changes = append(changes, "desbloqueio")
	}
	if len(fields) > 0 {
		writeError(w, http.StatusBadRequest, "validation", "Confira os campos destacados.", fields)
		return
	}
	if len(set) > 0 {
		if err := s.store.UpdateUser(ctx, email, set); err != nil {
			s.fail(w, r, err)
			return
		}
		s.audit(r, "usuario_alterado", email+": "+strings.Join(changes, ", "))
	}
	u, err := s.store.User(ctx, email)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, viewOf(u, s.store.Now()))
}

func (s *Server) handleUserDelete(w http.ResponseWriter, r *http.Request) {
	email := strings.ToLower(strings.TrimSpace(r.PathValue("email")))
	me := currentUser(r)
	if email == me.Email {
		writeError(w, http.StatusConflict, "conflict", "Você não pode remover a própria conta.", nil)
		return
	}
	ctx := r.Context()
	target, err := s.store.User(ctx, email)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if target.Role == auth.RoleAdmin {
		admins, err := s.store.CountAdmins(ctx)
		if err != nil {
			s.fail(w, r, err)
			return
		}
		if admins <= 1 {
			writeError(w, http.StatusConflict, "conflict", "É preciso manter pelo menos um administrador.", nil)
			return
		}
	}
	if err := s.store.DeleteUser(ctx, email); err != nil {
		s.fail(w, r, err)
		return
	}
	if err := s.store.DeleteTokensOf(ctx, email); err != nil {
		s.log.Warn("tokens do usuário removido não apagados", "email", email, "erro", err)
	}
	s.audit(r, "usuario_removido", email)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleAudit(w http.ResponseWriter, r *http.Request) {
	limit := queryInt(r, "limit", 100, 1, 1000)
	entries, err := s.store.AuditLog(r.Context(), limit)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, entries)
}

func queryInt(r *http.Request, name string, fallback, min, max int) int {
	v, err := strconv.Atoi(strings.TrimSpace(r.URL.Query().Get(name)))
	if err != nil {
		return fallback
	}
	if v < min {
		return min
	}
	if v > max {
		return max
	}
	return v
}

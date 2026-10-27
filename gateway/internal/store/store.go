package store

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"

	"arca/gateway/internal/auth"
	"arca/gateway/internal/resp"
)

const (
	UsersTable   = "_painel_usuarios"
	AuditTable   = "_painel_auditoria"
	SessionKey   = "_painel:sessao:"
	SystemPrefix = "_painel"
)

var ErrNotFound = errors.New("não encontrado")

type Store struct {
	Client *resp.Client
	Now    func() time.Time
}

func New(client *resp.Client) *Store {
	return &Store{Client: client, Now: time.Now}
}

type SQLResult struct {
	Kind     string   `json:"kind"`
	Columns  []string `json:"columns"`
	Types    []string `json:"types"`
	Rows     [][]any  `json:"rows"`
	Plan     string   `json:"plan"`
	Tag      string   `json:"tag"`
	Affected int64    `json:"affected"`
	Ms       float64  `json:"ms"`
}

func (r *SQLResult) Index(column string) int {
	for i, c := range r.Columns {
		if strings.EqualFold(c, column) {
			return i
		}
	}
	return -1
}

func (r *SQLResult) Maps() []map[string]any {
	out := make([]map[string]any, 0, len(r.Rows))
	for _, row := range r.Rows {
		m := make(map[string]any, len(r.Columns))
		for i, c := range r.Columns {
			if i < len(row) {
				m[c] = row[i]
			}
		}
		out = append(out, m)
	}
	return out
}

func decodeJSON(raw []byte, out any) error {
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	return dec.Decode(out)
}

func (s *Store) SQLRaw(ctx context.Context, query string) (json.RawMessage, error) {
	return s.Client.RawJSON(ctx, "SQL.JSON", query)
}

func (s *Store) SQL(ctx context.Context, query string) (*SQLResult, error) {
	raw, err := s.SQLRaw(ctx, query)
	if err != nil {
		return nil, err
	}
	var res SQLResult
	if err := decodeJSON(raw, &res); err != nil {
		return nil, fmt.Errorf("resposta SQL inválida: %w", err)
	}
	return &res, nil
}

func (s *Store) SQLf(ctx context.Context, format string, args ...any) (*SQLResult, error) {
	return s.SQL(ctx, fmt.Sprintf(format, args...))
}

type Column struct {
	Name          string `json:"name"`
	Type          string `json:"type"`
	PrimaryKey    bool   `json:"primary_key"`
	NotNull       bool   `json:"not_null"`
	Default       any    `json:"default"`
	AutoIncrement bool   `json:"auto_increment"`
}

type Index struct {
	Name   string `json:"name"`
	Column string `json:"column"`
}

type Table struct {
	Name    string   `json:"name"`
	Columns []Column `json:"columns"`
	Indexes []Index  `json:"indexes"`
	Rows    int64    `json:"rows"`
}

func (t *Table) Column(name string) *Column {
	for i := range t.Columns {
		if strings.EqualFold(t.Columns[i].Name, name) {
			return &t.Columns[i]
		}
	}
	return nil
}

func (t *Table) PrimaryKey() *Column {
	for i := range t.Columns {
		if t.Columns[i].PrimaryKey {
			return &t.Columns[i]
		}
	}
	return nil
}

func IsSystemTable(name string) bool {
	return strings.HasPrefix(name, "_")
}

func (s *Store) Tables(ctx context.Context) ([]Table, error) {
	raw, err := s.Client.RawJSON(ctx, "SQL.TABLES")
	if err != nil {
		return nil, err
	}
	var tables []Table
	if err := decodeJSON(raw, &tables); err != nil {
		return nil, fmt.Errorf("lista de tabelas inválida: %w", err)
	}
	for i := range tables {
		if tables[i].Columns == nil {
			tables[i].Columns = []Column{}
		}
		if tables[i].Indexes == nil {
			tables[i].Indexes = []Index{}
		}
	}
	return tables, nil
}

func (s *Store) UserTables(ctx context.Context) ([]Table, error) {
	all, err := s.Tables(ctx)
	if err != nil {
		return nil, err
	}
	out := make([]Table, 0, len(all))
	for _, t := range all {
		if !IsSystemTable(t.Name) {
			out = append(out, t)
		}
	}
	return out, nil
}

func (s *Store) Table(ctx context.Context, name string) (*Table, error) {
	tables, err := s.Tables(ctx)
	if err != nil {
		return nil, err
	}
	for i := range tables {
		if strings.EqualFold(tables[i].Name, name) {
			return &tables[i], nil
		}
	}
	return nil, ErrNotFound
}

func (s *Store) Stats(ctx context.Context) (json.RawMessage, error) {
	return s.Client.RawJSON(ctx, "ARCA.STATS")
}

func (s *Store) Bootstrap(ctx context.Context) error {
	stmts := []string{
		"CREATE TABLE IF NOT EXISTS " + UsersTable + " (email TEXT PRIMARY KEY, name TEXT NOT NULL, role TEXT NOT NULL, password TEXT NOT NULL, created_at INT, failed INT DEFAULT 0, locked_until INT DEFAULT 0)",
		"CREATE TABLE IF NOT EXISTS " + AuditTable + " (id INT PRIMARY KEY AUTOINCREMENT, at INT, email TEXT, action TEXT, detail TEXT)",
	}
	stmts = append(stmts, extraTables...)
	for _, q := range stmts {
		if _, err := s.SQL(ctx, q); err != nil {
			return err
		}
	}
	return nil
}

type User struct {
	Email       string `json:"email"`
	Name        string `json:"name"`
	Role        string `json:"role"`
	Password    string `json:"-"`
	CreatedAt   int64  `json:"created_at"`
	Failed      int64  `json:"-"`
	LockedUntil int64  `json:"-"`
	// TokenName é o nome do token de API usado na requisição (vazio numa sessão do painel).
	TokenName string `json:"-"`
}

func (u *User) Locked(now time.Time) bool {
	return u.LockedUntil > now.UnixMilli()
}

func userFromRow(m map[string]any) User {
	return User{
		Email:       Cell(m["email"]),
		Name:        Cell(m["name"]),
		Role:        Cell(m["role"]),
		Password:    Cell(m["password"]),
		CreatedAt:   CellInt(m["created_at"]),
		Failed:      CellInt(m["failed"]),
		LockedUntil: CellInt(m["locked_until"]),
	}
}

const userColumns = "email, name, role, password, created_at, failed, locked_until"

func (s *Store) User(ctx context.Context, email string) (*User, error) {
	res, err := s.SQLf(ctx, "SELECT %s FROM %s WHERE email = %s", userColumns, UsersTable, Quote(email))
	if err != nil {
		return nil, err
	}
	rows := res.Maps()
	if len(rows) == 0 {
		return nil, ErrNotFound
	}
	u := userFromRow(rows[0])
	return &u, nil
}

func (s *Store) Users(ctx context.Context) ([]User, error) {
	res, err := s.SQLf(ctx, "SELECT %s FROM %s ORDER BY email", userColumns, UsersTable)
	if err != nil {
		return nil, err
	}
	rows := res.Maps()
	out := make([]User, 0, len(rows))
	for _, m := range rows {
		out = append(out, userFromRow(m))
	}
	return out, nil
}

func (s *Store) CountUsers(ctx context.Context) (int64, error) {
	res, err := s.SQLf(ctx, "SELECT COUNT(*) FROM %s", UsersTable)
	if err != nil {
		return 0, err
	}
	if len(res.Rows) == 0 || len(res.Rows[0]) == 0 {
		return 0, nil
	}
	return CellInt(res.Rows[0][0]), nil
}

func (s *Store) CountAdmins(ctx context.Context) (int64, error) {
	res, err := s.SQLf(ctx, "SELECT COUNT(*) FROM %s WHERE role = %s", UsersTable, Quote(auth.RoleAdmin))
	if err != nil {
		return 0, err
	}
	if len(res.Rows) == 0 || len(res.Rows[0]) == 0 {
		return 0, nil
	}
	return CellInt(res.Rows[0][0]), nil
}

func (s *Store) CreateUser(ctx context.Context, email, name, role, password string) error {
	hash, err := auth.HashPassword(password)
	if err != nil {
		return err
	}
	_, err = s.SQLf(ctx, "INSERT INTO %s (email, name, role, password, created_at, failed, locked_until) VALUES (%s, %s, %s, %s, %d, 0, 0)",
		UsersTable, Quote(email), Quote(name), Quote(role), Quote(hash), s.Now().UnixMilli())
	return err
}

func (s *Store) UpdateUser(ctx context.Context, email string, set map[string]string) error {
	if len(set) == 0 {
		return nil
	}
	parts := make([]string, 0, len(set))
	for _, col := range []string{"name", "role", "password", "failed", "locked_until"} {
		if v, ok := set[col]; ok {
			parts = append(parts, col+" = "+v)
		}
	}
	res, err := s.SQLf(ctx, "UPDATE %s SET %s WHERE email = %s", UsersTable, strings.Join(parts, ", "), Quote(email))
	if err != nil {
		return err
	}
	if res.Affected == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) SetPassword(ctx context.Context, email, password string) error {
	hash, err := auth.HashPassword(password)
	if err != nil {
		return err
	}
	return s.UpdateUser(ctx, email, map[string]string{"password": Quote(hash), "failed": "0", "locked_until": "0"})
}

func (s *Store) DeleteUser(ctx context.Context, email string) error {
	res, err := s.SQLf(ctx, "DELETE FROM %s WHERE email = %s", UsersTable, Quote(email))
	if err != nil {
		return err
	}
	if res.Affected == 0 {
		return ErrNotFound
	}
	return nil
}

type session struct {
	Email     string `json:"email"`
	CreatedAt int64  `json:"created_at"`
}

func (s *Store) CreateSession(ctx context.Context, token, email string, ttl time.Duration) error {
	payload, _ := json.Marshal(session{Email: email, CreatedAt: s.Now().UnixMilli()})
	secs := int64(ttl / time.Second)
	if secs < 1 {
		secs = 1
	}
	_, err := s.Client.Check(ctx, "SET", SessionKey+auth.TokenHash(token), string(payload), "EX", strconv.FormatInt(secs, 10))
	return err
}

func (s *Store) SessionEmail(ctx context.Context, token string) (string, error) {
	v, err := s.Client.Check(ctx, "GET", SessionKey+auth.TokenHash(token))
	if err != nil {
		return "", err
	}
	if v.IsNull() {
		return "", ErrNotFound
	}
	var sess session
	if err := json.Unmarshal([]byte(v.Str), &sess); err != nil || sess.Email == "" {
		return "", ErrNotFound
	}
	return sess.Email, nil
}

func (s *Store) DeleteSession(ctx context.Context, token string) error {
	_, err := s.Client.Check(ctx, "DEL", SessionKey+auth.TokenHash(token))
	return err
}

type AuditEntry struct {
	ID     int64  `json:"id"`
	At     int64  `json:"at"`
	Email  string `json:"email"`
	Action string `json:"action"`
	Detail string `json:"detail"`
}

func (s *Store) Audit(ctx context.Context, email, action, detail string) error {
	if len(detail) > 600 {
		detail = detail[:600] + "…"
	}
	_, err := s.SQLf(ctx, "INSERT INTO %s (at, email, action, detail) VALUES (%d, %s, %s, %s)",
		AuditTable, s.Now().UnixMilli(), Quote(email), Quote(action), Quote(detail))
	return err
}

func (s *Store) AuditLog(ctx context.Context, limit int) ([]AuditEntry, error) {
	res, err := s.SQLf(ctx, "SELECT id, at, email, action, detail FROM %s ORDER BY id DESC LIMIT %d", AuditTable, limit)
	if err != nil {
		return nil, err
	}
	out := make([]AuditEntry, 0, len(res.Rows))
	for _, m := range res.Maps() {
		out = append(out, AuditEntry{
			ID:     CellInt(m["id"]),
			At:     CellInt(m["at"]),
			Email:  Cell(m["email"]),
			Action: Cell(m["action"]),
			Detail: Cell(m["detail"]),
		})
	}
	return out, nil
}

package store

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"strconv"
	"strings"
)

const (
	TokensTable     = "_painel_tokens"
	WebhooksTable   = "_painel_webhooks"
	DeliveriesTable = "_painel_webhook_entregas"
	QueriesTable    = "_painel_consultas"
)

var extraTables = []string{
	"CREATE TABLE IF NOT EXISTS " + TokensTable + " (id TEXT PRIMARY KEY, hash TEXT NOT NULL, name TEXT NOT NULL, role TEXT NOT NULL, owner TEXT NOT NULL, prefix TEXT, created_at INT, last_used_at INT DEFAULT 0, expires_at INT DEFAULT 0)",
	"CREATE INDEX IF NOT EXISTS _painel_tokens_hash ON " + TokensTable + " (hash)",
	"CREATE TABLE IF NOT EXISTS " + WebhooksTable + " (id TEXT PRIMARY KEY, url TEXT NOT NULL, events TEXT NOT NULL, active INT DEFAULT 1, secret TEXT NOT NULL, created_at INT, last_status INT DEFAULT 0, last_delivery_at INT DEFAULT 0, failures INT DEFAULT 0)",
	"CREATE TABLE IF NOT EXISTS " + DeliveriesTable + " (id INT PRIMARY KEY AUTOINCREMENT, delivery TEXT, webhook TEXT NOT NULL, at INT, event TEXT, status INT, ms REAL, error TEXT)",
	"CREATE INDEX IF NOT EXISTS _painel_entregas_webhook ON " + DeliveriesTable + " (webhook)",
	"CREATE TABLE IF NOT EXISTS " + QueriesTable + " (id TEXT PRIMARY KEY, name TEXT NOT NULL, query TEXT NOT NULL, owner TEXT NOT NULL, shared INT DEFAULT 0, created_at INT)",
}

// NewID devolve um identificador aleatório de 16 caracteres hexadecimais.
func NewID() string {
	b := make([]byte, 8)
	rand.Read(b)
	return hex.EncodeToString(b)
}

func boolInt(b bool) string {
	if b {
		return "1"
	}
	return "0"
}

// ---------- tokens de API ----------

type APIToken struct {
	ID         string `json:"id"`
	Hash       string `json:"-"`
	Name       string `json:"name"`
	Role       string `json:"role"`
	Owner      string `json:"owner"`
	Prefix     string `json:"prefix"`
	CreatedAt  int64  `json:"created_at"`
	LastUsedAt int64  `json:"-"`
	ExpiresAt  int64  `json:"-"`
}

const tokenColumns = "id, hash, name, role, owner, prefix, created_at, last_used_at, expires_at"

func tokenFromRow(m map[string]any) APIToken {
	return APIToken{
		ID:         Cell(m["id"]),
		Hash:       Cell(m["hash"]),
		Name:       Cell(m["name"]),
		Role:       Cell(m["role"]),
		Owner:      Cell(m["owner"]),
		Prefix:     Cell(m["prefix"]),
		CreatedAt:  CellInt(m["created_at"]),
		LastUsedAt: CellInt(m["last_used_at"]),
		ExpiresAt:  CellInt(m["expires_at"]),
	}
}

func (s *Store) tokens(ctx context.Context, where string) ([]APIToken, error) {
	res, err := s.SQLf(ctx, "SELECT %s FROM %s%s", tokenColumns, TokensTable, where)
	if err != nil {
		return nil, err
	}
	out := make([]APIToken, 0, len(res.Rows))
	for _, m := range res.Maps() {
		out = append(out, tokenFromRow(m))
	}
	return out, nil
}

func (s *Store) CreateToken(ctx context.Context, t APIToken) error {
	_, err := s.SQLf(ctx, "INSERT INTO %s (%s) VALUES (%s, %s, %s, %s, %s, %s, %d, 0, %d)", TokensTable, tokenColumns,
		Quote(t.ID), Quote(t.Hash), Quote(t.Name), Quote(t.Role), Quote(t.Owner), Quote(t.Prefix), t.CreatedAt, t.ExpiresAt)
	return err
}

func (s *Store) TokenByHash(ctx context.Context, hash string) (*APIToken, error) {
	list, err := s.tokens(ctx, " WHERE hash = "+Quote(hash))
	if err != nil {
		return nil, err
	}
	if len(list) == 0 {
		return nil, ErrNotFound
	}
	return &list[0], nil
}

func (s *Store) Token(ctx context.Context, id string) (*APIToken, error) {
	list, err := s.tokens(ctx, " WHERE id = "+Quote(id))
	if err != nil {
		return nil, err
	}
	if len(list) == 0 {
		return nil, ErrNotFound
	}
	return &list[0], nil
}

// Tokens lista os tokens de um dono (owner vazio lista todos).
func (s *Store) Tokens(ctx context.Context, owner string) ([]APIToken, error) {
	where := ""
	if owner != "" {
		where = " WHERE owner = " + Quote(owner)
	}
	return s.tokens(ctx, where)
}

func (s *Store) TouchToken(ctx context.Context, id string, at int64) error {
	_, err := s.SQLf(ctx, "UPDATE %s SET last_used_at = %d WHERE id = %s", TokensTable, at, Quote(id))
	return err
}

func (s *Store) DeleteToken(ctx context.Context, id string) error {
	res, err := s.SQLf(ctx, "DELETE FROM %s WHERE id = %s", TokensTable, Quote(id))
	if err != nil {
		return err
	}
	if res.Affected == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) DeleteTokensOf(ctx context.Context, owner string) error {
	_, err := s.SQLf(ctx, "DELETE FROM %s WHERE owner = %s", TokensTable, Quote(owner))
	return err
}

// ---------- webhooks ----------

type Webhook struct {
	ID             string
	URL            string
	Events         []string
	Active         bool
	Secret         string
	CreatedAt      int64
	LastStatus     int64
	LastDeliveryAt int64
	Failures       int64
}

func (w *Webhook) Wants(event string) bool {
	for _, e := range w.Events {
		if e == event {
			return true
		}
	}
	return false
}

const webhookColumns = "id, url, events, active, secret, created_at, last_status, last_delivery_at, failures"

func splitEvents(s string) []string {
	out := []string{}
	for _, e := range strings.Split(s, ",") {
		if e = strings.TrimSpace(e); e != "" {
			out = append(out, e)
		}
	}
	return out
}

func webhookFromRow(m map[string]any) Webhook {
	return Webhook{
		ID:             Cell(m["id"]),
		URL:            Cell(m["url"]),
		Events:         splitEvents(Cell(m["events"])),
		Active:         CellInt(m["active"]) != 0,
		Secret:         Cell(m["secret"]),
		CreatedAt:      CellInt(m["created_at"]),
		LastStatus:     CellInt(m["last_status"]),
		LastDeliveryAt: CellInt(m["last_delivery_at"]),
		Failures:       CellInt(m["failures"]),
	}
}

func (s *Store) Webhooks(ctx context.Context) ([]Webhook, error) {
	res, err := s.SQLf(ctx, "SELECT %s FROM %s", webhookColumns, WebhooksTable)
	if err != nil {
		return nil, err
	}
	out := make([]Webhook, 0, len(res.Rows))
	for _, m := range res.Maps() {
		out = append(out, webhookFromRow(m))
	}
	return out, nil
}

func (s *Store) Webhook(ctx context.Context, id string) (*Webhook, error) {
	res, err := s.SQLf(ctx, "SELECT %s FROM %s WHERE id = %s", webhookColumns, WebhooksTable, Quote(id))
	if err != nil {
		return nil, err
	}
	rows := res.Maps()
	if len(rows) == 0 {
		return nil, ErrNotFound
	}
	w := webhookFromRow(rows[0])
	return &w, nil
}

func (s *Store) CreateWebhook(ctx context.Context, w Webhook) error {
	_, err := s.SQLf(ctx, "INSERT INTO %s (%s) VALUES (%s, %s, %s, %s, %s, %d, 0, 0, 0)", WebhooksTable, webhookColumns,
		Quote(w.ID), Quote(w.URL), Quote(strings.Join(w.Events, ",")), boolInt(w.Active), Quote(w.Secret), w.CreatedAt)
	return err
}

func (s *Store) UpdateWebhook(ctx context.Context, w Webhook) error {
	res, err := s.SQLf(ctx, "UPDATE %s SET url = %s, events = %s, active = %s, secret = %s WHERE id = %s", WebhooksTable,
		Quote(w.URL), Quote(strings.Join(w.Events, ",")), boolInt(w.Active), Quote(w.Secret), Quote(w.ID))
	if err != nil {
		return err
	}
	if res.Affected == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) WebhookResult(ctx context.Context, id string, status, at, failures int64) error {
	_, err := s.SQLf(ctx, "UPDATE %s SET last_status = %d, last_delivery_at = %d, failures = %d WHERE id = %s", WebhooksTable, status, at, failures, Quote(id))
	return err
}

func (s *Store) DeleteWebhook(ctx context.Context, id string) error {
	res, err := s.SQLf(ctx, "DELETE FROM %s WHERE id = %s", WebhooksTable, Quote(id))
	if err != nil {
		return err
	}
	if res.Affected == 0 {
		return ErrNotFound
	}
	_, err = s.SQLf(ctx, "DELETE FROM %s WHERE webhook = %s", DeliveriesTable, Quote(id))
	return err
}

type Delivery struct {
	ID     string  `json:"id"`
	At     int64   `json:"at"`
	Event  string  `json:"event"`
	Status int64   `json:"status"`
	Ms     float64 `json:"ms"`
	Error  string  `json:"error,omitempty"`
}

// AddDelivery registra uma entrega e mantém só as `keep` mais recentes do webhook.
func (s *Store) AddDelivery(ctx context.Context, webhook string, d Delivery, keep int) error {
	if len(d.Error) > 300 {
		d.Error = d.Error[:300] + "…"
	}
	ms, err := RealLiteral(d.Ms)
	if err != nil {
		ms = "0.0"
	}
	if _, err := s.SQLf(ctx, "INSERT INTO %s (delivery, webhook, at, event, status, ms, error) VALUES (%s, %s, %d, %s, %d, %s, %s)", DeliveriesTable,
		Quote(d.ID), Quote(webhook), d.At, Quote(d.Event), d.Status, ms, Quote(d.Error)); err != nil {
		return err
	}
	res, err := s.SQLf(ctx, "SELECT id FROM %s WHERE webhook = %s ORDER BY id DESC LIMIT 1 OFFSET %d", DeliveriesTable, Quote(webhook), keep)
	if err != nil {
		return err
	}
	if len(res.Rows) > 0 && len(res.Rows[0]) > 0 {
		cut := CellInt(res.Rows[0][0])
		_, err = s.SQLf(ctx, "DELETE FROM %s WHERE webhook = %s AND id <= %d", DeliveriesTable, Quote(webhook), cut)
	}
	return err
}

func (s *Store) Deliveries(ctx context.Context, webhook string, limit int) ([]Delivery, error) {
	res, err := s.SQLf(ctx, "SELECT delivery, at, event, status, ms, error FROM %s WHERE webhook = %s ORDER BY id DESC LIMIT %d", DeliveriesTable, Quote(webhook), limit)
	if err != nil {
		return nil, err
	}
	out := make([]Delivery, 0, len(res.Rows))
	for _, m := range res.Maps() {
		ms, _ := strconv.ParseFloat(Cell(m["ms"]), 64)
		out = append(out, Delivery{ID: Cell(m["delivery"]), At: CellInt(m["at"]), Event: Cell(m["event"]), Status: CellInt(m["status"]), Ms: ms, Error: Cell(m["error"])})
	}
	return out, nil
}

// ---------- consultas salvas ----------

type SavedQuery struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	Query     string `json:"query"`
	Owner     string `json:"owner"`
	Shared    bool   `json:"shared"`
	CreatedAt int64  `json:"created_at"`
}

const queryColumns = "id, name, query, owner, shared, created_at"

func (s *Store) Queries(ctx context.Context) ([]SavedQuery, error) {
	res, err := s.SQLf(ctx, "SELECT %s FROM %s ORDER BY created_at DESC", queryColumns, QueriesTable)
	if err != nil {
		return nil, err
	}
	out := make([]SavedQuery, 0, len(res.Rows))
	for _, m := range res.Maps() {
		out = append(out, SavedQuery{ID: Cell(m["id"]), Name: Cell(m["name"]), Query: Cell(m["query"]), Owner: Cell(m["owner"]), Shared: CellInt(m["shared"]) != 0, CreatedAt: CellInt(m["created_at"])})
	}
	return out, nil
}

func (s *Store) CreateQuery(ctx context.Context, q SavedQuery) error {
	_, err := s.SQLf(ctx, "INSERT INTO %s (%s) VALUES (%s, %s, %s, %s, %s, %d)", QueriesTable, queryColumns,
		Quote(q.ID), Quote(q.Name), Quote(q.Query), Quote(q.Owner), boolInt(q.Shared), q.CreatedAt)
	return err
}

func (s *Store) DeleteQuery(ctx context.Context, id string) error {
	res, err := s.SQLf(ctx, "DELETE FROM %s WHERE id = %s", QueriesTable, Quote(id))
	if err != nil {
		return err
	}
	if res.Affected == 0 {
		return ErrNotFound
	}
	return nil
}

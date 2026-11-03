package api

import (
	"bytes"
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"arca/gateway/internal/store"
)

const maxBulkDelete = 50000

func (s *Server) handleKeyNamespaces(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	sep := q.Get("sep")
	if sep == "" {
		sep = ":"
	}
	if len(sep) > 8 || strings.ContainsAny(sep, " \t\r\n") {
		writeError(w, http.StatusBadRequest, "validation", "O separador precisa ter de 1 a 8 caracteres, sem espaços.", map[string]string{"sep": "inválido"})
		return
	}
	limit := queryInt(r, "limit", 50, 1, 1000)
	v, err := s.client.DoTimeout(r.Context(), 2*time.Minute, "ARCA.NAMESPACES", "SEP", sep, "LIMIT", strconv.Itoa(limit), "EXCLUDE", store.SystemPrefix+":")
	if err == nil {
		err = v.Err()
	}
	if err != nil {
		s.fail(w, r, err)
		return
	}
	raw := []byte(strings.TrimSpace(v.Text()))
	if !json.Valid(raw) {
		s.fail(w, r, errors.New("resposta de ARCA.NAMESPACES inválida"))
		return
	}
	writeRaw(w, http.StatusOK, raw)
}

type bulkDelete struct {
	Match  string `json:"match"`
	DryRun bool   `json:"dry_run"`
}

func (s *Server) handleKeysDeleteMatch(w http.ResponseWriter, r *http.Request) {
	var req bulkDelete
	if !decodeBody(w, r, &req) {
		return
	}
	req.Match = strings.TrimSpace(req.Match)
	if req.Match == "" || req.Match == "*" {
		writeError(w, http.StatusBadRequest, "validation", "Informe um padrão mais específico que *, por exemplo sessao:*.", map[string]string{"match": "muito amplo"})
		return
	}
	if len(req.Match) > 1024 {
		writeError(w, http.StatusBadRequest, "validation", "O padrão pode ter no máximo 1024 bytes.", map[string]string{"match": "muito longo"})
		return
	}
	ctx := r.Context()
	cursor := "0"
	keys := []string{}
	truncated := false
	for {
		var page scanResult
		if err := s.client.JSON(ctx, &page, "ARCA.SCANJSON", cursor, req.Match, "1000"); err != nil {
			s.fail(w, r, err)
			return
		}
		for _, k := range page.Keys {
			if protectedKey(k.Key) {
				continue
			}
			if len(keys) >= maxBulkDelete {
				truncated = true
				break
			}
			keys = append(keys, k.Key)
		}
		if truncated || page.Cursor == "" || page.Cursor == "0" {
			break
		}
		cursor = page.Cursor
	}
	if req.DryRun {
		sample := keys
		if len(sample) > 20 {
			sample = sample[:20]
		}
		writeJSON(w, http.StatusOK, map[string]any{"match": req.Match, "matched": len(keys), "truncated": truncated, "sample": sample})
		return
	}
	var deleted int64
	for start := 0; start < len(keys); start += 500 {
		end := start + 500
		if end > len(keys) {
			end = len(keys)
		}
		n, err := s.client.Int(ctx, append([]string{"DEL"}, keys[start:end]...)...)
		if err != nil {
			s.fail(w, r, err)
			return
		}
		deleted += n
	}
	if deleted > 0 {
		s.audit(r, "chaves_removidas", fmt.Sprintf("%s (%d chaves)", req.Match, deleted))
	}
	writeJSON(w, http.StatusOK, map[string]any{"match": req.Match, "deleted": deleted, "truncated": truncated})
}

func (s *Server) handleAuditExport(w http.ResponseWriter, r *http.Request) {
	limit := queryInt(r, "limit", 1000, 1, 10000)
	entries, err := s.store.AuditLog(r.Context(), limit)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	stamp := time.Now().Format("20060102-1504")
	if strings.EqualFold(r.URL.Query().Get("format"), "json") {
		attachment(w, "arca-auditoria-"+stamp+".json", "application/json; charset=utf-8")
		writeJSON(w, http.StatusOK, entries)
		return
	}
	var buf bytes.Buffer
	cw := csv.NewWriter(&buf)
	cw.Write([]string{"id", "data", "email", "acao", "detalhe"})
	for _, e := range entries {
		cw.Write([]string{
			strconv.FormatInt(e.ID, 10),
			time.UnixMilli(e.At).UTC().Format(time.RFC3339),
			e.Email,
			e.Action,
			e.Detail,
		})
	}
	cw.Flush()
	attachment(w, "arca-auditoria-"+stamp+".csv", "text/csv; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	w.Write(buf.Bytes())
}

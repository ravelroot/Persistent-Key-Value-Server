package api

import (
	"bytes"
	"encoding/base64"
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"arca/gateway/internal/resp"
	"arca/gateway/internal/store"
)

const (
	maxExportKeys   = 100000
	maxImportRows   = 100000
	importBodyLimit = 16 << 20
	maxImportErrors = 20
)

type exportedKey struct {
	Key    string `json:"key"`
	Value  string `json:"value"`
	TTLMs  *int64 `json:"ttl_ms"`
	Base64 bool   `json:"base64,omitempty"`
}

func attachment(w http.ResponseWriter, filename, contentType string) {
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("Content-Disposition", `attachment; filename="`+filename+`"`)
}

func (s *Server) handleKeysExport(w http.ResponseWriter, r *http.Request) {
	match := r.URL.Query().Get("match")
	if match == "" {
		match = "*"
	}
	ctx := r.Context()
	cursor := "0"
	keys := []exportedKey{}
	truncated := false
	for {
		var page scanResult
		if err := s.client.JSON(ctx, &page, "ARCA.SCANJSON", cursor, match, "1000"); err != nil {
			s.fail(w, r, err)
			return
		}
		names := make([]string, 0, len(page.Keys))
		ttls := make([]int64, 0, len(page.Keys))
		for _, k := range page.Keys {
			if !protectedKey(k.Key) {
				names = append(names, k.Key)
				ttls = append(ttls, k.TTLMs)
			}
		}
		if len(names) > 0 {
			v, err := s.client.Check(ctx, append([]string{"MGET"}, names...)...)
			if err != nil {
				s.fail(w, r, err)
				return
			}
			for i, item := range v.Array {
				if i >= len(names) || item.IsNull() {
					continue
				}
				e := exportedKey{Key: names[i], Value: item.Str}
				if !utf8.ValidString(item.Str) {
					e.Value = base64.StdEncoding.EncodeToString([]byte(item.Str))
					e.Base64 = true
				}
				if ttls[i] > 0 {
					t := ttls[i]
					e.TTLMs = &t
				}
				keys = append(keys, e)
				if len(keys) >= maxExportKeys {
					truncated = true
					break
				}
			}
		}
		if truncated || page.Cursor == "" || page.Cursor == "0" {
			break
		}
		cursor = page.Cursor
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i].Key < keys[j].Key })
	now := time.Now()
	attachment(w, "arca-chaves-"+now.Format("20060102-1504")+".json", "application/json; charset=utf-8")
	writeJSON(w, http.StatusOK, map[string]any{
		"format":      "arca.keys",
		"version":     1,
		"exported_at": now.UnixMilli(),
		"match":       match,
		"count":       len(keys),
		"truncated":   truncated,
		"keys":        keys,
	})
}

type importKey struct {
	Key    string       `json:"key"`
	Value  *string      `json:"value"`
	TTLMs  *json.Number `json:"ttl_ms"`
	Base64 bool         `json:"base64"`
}

type keysImport struct {
	Keys []importKey `json:"keys"`
	Mode string      `json:"mode"`
}

func (s *Server) handleKeysImport(w http.ResponseWriter, r *http.Request) {
	var req keysImport
	if !decodeBodyLimit(w, r, &req, importBodyLimit) {
		return
	}
	mode := strings.ToLower(strings.TrimSpace(req.Mode))
	if mode == "" {
		mode = "overwrite"
	}
	if mode != "overwrite" && mode != "skip" {
		writeError(w, http.StatusBadRequest, "validation", "Modo inválido.", map[string]string{"mode": "use overwrite ou skip"})
		return
	}
	if len(req.Keys) == 0 {
		writeError(w, http.StatusBadRequest, "validation", "O arquivo não tem chaves para importar.", map[string]string{"keys": "vazio"})
		return
	}
	if len(req.Keys) > maxImportRows {
		writeError(w, http.StatusBadRequest, "validation", "Importe no máximo 100 000 chaves por vez.", map[string]string{"keys": "muitas chaves"})
		return
	}
	ctx := r.Context()
	var imported, skipped, failed int
	for _, k := range req.Keys {
		if k.Key == "" || len(k.Key) > 4096 || protectedKey(k.Key) || k.Value == nil {
			failed++
			continue
		}
		value := *k.Value
		if k.Base64 {
			raw, err := base64.StdEncoding.DecodeString(value)
			if err != nil {
				failed++
				continue
			}
			value = string(raw)
		}
		args := []string{"SET", k.Key, value}
		if k.TTLMs != nil && k.TTLMs.String() != "" {
			ttl, err := strconv.ParseInt(k.TTLMs.String(), 10, 64)
			if err != nil {
				failed++
				continue
			}
			if ttl > 0 {
				args = append(args, "PX", strconv.FormatInt(ttl, 10))
			}
		}
		if mode == "skip" {
			args = append(args, "NX")
		}
		v, err := s.client.Do(ctx, args...)
		if err != nil {
			s.fail(w, r, err)
			return
		}
		switch {
		case v.Err() != nil:
			failed++
		case v.IsNull():
			skipped++
		default:
			imported++
		}
	}
	s.audit(r, "chaves_importadas", fmt.Sprintf("%d importadas, %d ignoradas, %d com erro", imported, skipped, failed))
	writeJSON(w, http.StatusOK, map[string]int{"imported": imported, "skipped": skipped, "failed": failed})
}

func (s *Server) tableRows(r *http.Request, t *store.Table, each func(columns []string, row []any) error) ([]string, error) {
	const page = 1000
	var columns []string
	for offset := 0; ; offset += page {
		res, err := s.store.SQLf(r.Context(), "SELECT * FROM %s LIMIT %d OFFSET %d", t.Name, page, offset)
		if err != nil {
			return nil, err
		}
		if columns == nil {
			columns = res.Columns
		}
		for _, row := range res.Rows {
			if err := each(res.Columns, row); err != nil {
				return nil, err
			}
		}
		if len(res.Rows) < page {
			break
		}
	}
	if columns == nil {
		for _, c := range t.Columns {
			columns = append(columns, c.Name)
		}
	}
	return columns, nil
}

func (s *Server) handleTableExport(w http.ResponseWriter, r *http.Request) {
	t, ok := s.userTable(w, r)
	if !ok {
		return
	}
	format := strings.ToLower(r.URL.Query().Get("format"))
	if format == "" {
		format = "csv"
	}
	if format != "csv" && format != "json" {
		writeError(w, http.StatusBadRequest, "validation", "Formato inválido.", map[string]string{"format": "use csv ou json"})
		return
	}
	var buf bytes.Buffer
	if format == "csv" {
		var rows [][]string
		columns, err := s.tableRows(r, t, func(_ []string, row []any) error {
			line := make([]string, len(row))
			for i, v := range row {
				line[i] = store.Cell(v)
			}
			rows = append(rows, line)
			return nil
		})
		if err != nil {
			s.fail(w, r, err)
			return
		}
		cw := csv.NewWriter(&buf)
		cw.Write(columns)
		cw.WriteAll(rows)
		attachment(w, t.Name+".csv", "text/csv; charset=utf-8")
		w.WriteHeader(http.StatusOK)
		w.Write(buf.Bytes())
		return
	}
	buf.WriteByte('[')
	n := 0
	_, err := s.tableRows(r, t, func(columns []string, row []any) error {
		if n > 0 {
			buf.WriteByte(',')
		}
		n++
		buf.WriteByte('{')
		for i, c := range columns {
			if i > 0 {
				buf.WriteByte(',')
			}
			k, _ := json.Marshal(c)
			buf.Write(k)
			buf.WriteByte(':')
			var v any
			if i < len(row) {
				v = row[i]
			}
			b, err := json.Marshal(v)
			if err != nil {
				return err
			}
			buf.Write(b)
		}
		buf.WriteByte('}')
		return nil
	})
	if err != nil {
		s.fail(w, r, err)
		return
	}
	buf.WriteString("]\n")
	attachment(w, t.Name+".json", "application/json; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	w.Write(buf.Bytes())
}

type tableImport struct {
	CSV  *string          `json:"csv"`
	Rows []map[string]any `json:"rows"`
}

type importError struct {
	Line    int    `json:"line"`
	Message string `json:"message"`
}

func fieldsMessage(fields map[string]string) string {
	names := make([]string, 0, len(fields))
	for k := range fields {
		names = append(names, k)
	}
	sort.Strings(names)
	parts := make([]string, 0, len(names))
	for _, k := range names {
		parts = append(parts, k+": "+fields[k])
	}
	return strings.Join(parts, "; ")
}

func parseCSV(text string) ([][]string, error) {
	text = strings.TrimPrefix(text, "\ufeff")
	first := text
	if i := strings.IndexAny(text, "\r\n"); i >= 0 {
		first = text[:i]
	}
	cr := csv.NewReader(strings.NewReader(text))
	cr.LazyQuotes = true
	cr.FieldsPerRecord = -1
	if strings.Count(first, ";") > strings.Count(first, ",") {
		cr.Comma = ';'
	}
	var out [][]string
	for {
		rec, err := cr.Read()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return nil, err
		}
		out = append(out, rec)
	}
	return out, nil
}

func (s *Server) handleTableImport(w http.ResponseWriter, r *http.Request) {
	t, ok := s.userTable(w, r)
	if !ok {
		return
	}
	var req tableImport
	if !decodeBodyLimit(w, r, &req, importBodyLimit) {
		return
	}
	type item struct {
		line   int
		values map[string]any
		err    string
	}
	var items []item
	switch {
	case req.CSV != nil:
		records, err := parseCSV(*req.CSV)
		if err != nil {
			writeError(w, http.StatusBadRequest, "validation", "Não foi possível ler o CSV: "+err.Error(), map[string]string{"csv": "inválido"})
			return
		}
		if len(records) < 2 {
			writeError(w, http.StatusBadRequest, "validation", "O CSV precisa de um cabeçalho com os nomes das colunas e ao menos uma linha.", map[string]string{"csv": "vazio"})
			return
		}
		header := records[0]
		var unknown []string
		cols := make([]*store.Column, len(header))
		for i, h := range header {
			h = strings.TrimSpace(h)
			c := t.Column(h)
			if c == nil {
				unknown = append(unknown, h)
				continue
			}
			cols[i] = c
		}
		if len(unknown) > 0 {
			writeError(w, http.StatusBadRequest, "validation", "O cabeçalho tem colunas que não existem em "+t.Name+": "+strings.Join(unknown, ", ")+".", map[string]string{"csv": "colunas desconhecidas"})
			return
		}
		for n, rec := range records[1:] {
			it := item{line: n + 2, values: map[string]any{}}
			if len(rec) != len(header) {
				it.err = fmt.Sprintf("esperadas %d colunas, vieram %d", len(header), len(rec))
			} else {
				for i, v := range rec {
					c := cols[i]
					if v == "" && !(c.NotNull && strings.EqualFold(c.Type, "TEXT")) {
						it.values[c.Name] = nil
					} else {
						it.values[c.Name] = v
					}
				}
			}
			items = append(items, it)
		}
	case len(req.Rows) > 0:
		for n, row := range req.Rows {
			items = append(items, item{line: n + 1, values: row})
		}
	default:
		writeError(w, http.StatusBadRequest, "validation", "Envie o conteúdo em csv ou a lista rows.", map[string]string{"csv": "obrigatório"})
		return
	}
	if len(items) > maxImportRows {
		writeError(w, http.StatusBadRequest, "validation", "Importe no máximo 100 000 linhas por vez.", nil)
		return
	}
	inserted, failed := 0, 0
	errs := []importError{}
	addErr := func(line int, msg string) {
		failed++
		if len(errs) < maxImportErrors {
			errs = append(errs, importError{Line: line, Message: msg})
		}
	}
	ctx := r.Context()
	for _, it := range items {
		if it.err != "" {
			addErr(it.line, it.err)
			continue
		}
		cols, lits, fields := columnsAndLiterals(t, it.values, false)
		if len(fields) > 0 {
			addErr(it.line, fieldsMessage(fields))
			continue
		}
		if len(cols) == 0 {
			addErr(it.line, "linha vazia")
			continue
		}
		_, err := s.store.SQLf(ctx, "INSERT INTO %s (%s) VALUES (%s)", t.Name, strings.Join(cols, ", "), strings.Join(lits, ", "))
		if err != nil {
			var serverErr *resp.ServerError
			if errors.As(err, &serverErr) {
				addErr(it.line, serverErr.Text())
				continue
			}
			s.fail(w, r, err)
			return
		}
		inserted++
	}
	if inserted > 0 || failed > 0 {
		s.audit(r, "linhas_importadas", fmt.Sprintf("%s: %d inseridas, %d com erro", t.Name, inserted, failed))
	}
	writeJSON(w, http.StatusOK, map[string]any{"inserted": inserted, "failed": failed, "errors": errs})
}

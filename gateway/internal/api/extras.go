package api

import (
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"arca/gateway/internal/auth"
	"arca/gateway/internal/resp"
	"arca/gateway/internal/store"
)

// ---------- consultas salvas ----------

const maxQueriesPerUser = 200

func (s *Server) handleQueries(w http.ResponseWriter, r *http.Request) {
	u := currentUser(r)
	all, err := s.store.Queries(r.Context())
	if err != nil {
		s.fail(w, r, err)
		return
	}
	out := make([]store.SavedQuery, 0, len(all))
	for _, q := range all {
		if q.Owner == u.Email || q.Shared {
			out = append(out, q)
		}
	}
	writeJSON(w, http.StatusOK, out)
}

type queryCreate struct {
	Name   string `json:"name"`
	Query  string `json:"query"`
	Shared bool   `json:"shared"`
}

func (s *Server) handleQueryCreate(w http.ResponseWriter, r *http.Request) {
	var req queryCreate
	if !decodeBody(w, r, &req) {
		return
	}
	fields := map[string]string{}
	name := strings.TrimSpace(req.Name)
	query := strings.TrimSpace(req.Query)
	if name == "" {
		fields["name"] = "obrigatório"
	} else if utf8.RuneCountInString(name) > 80 {
		fields["name"] = "no máximo 80 caracteres"
	}
	if query == "" {
		fields["query"] = "obrigatória"
	} else if len(query) > 64<<10 {
		fields["query"] = "no máximo 64 KB"
	}
	if len(fields) > 0 {
		writeError(w, http.StatusBadRequest, "validation", "Confira os campos destacados.", fields)
		return
	}
	u := currentUser(r)
	ctx := r.Context()
	all, err := s.store.Queries(ctx)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	mine := 0
	for _, q := range all {
		if q.Owner == u.Email {
			mine++
		}
	}
	if mine >= maxQueriesPerUser {
		writeError(w, http.StatusConflict, "conflict", "Você já tem 200 consultas salvas. Apague alguma antes de salvar outra.", nil)
		return
	}
	q := store.SavedQuery{ID: store.NewID(), Name: name, Query: query, Owner: u.Email, Shared: req.Shared, CreatedAt: s.store.Now().UnixMilli()}
	if err := s.store.CreateQuery(ctx, q); err != nil {
		s.fail(w, r, err)
		return
	}
	writeJSON(w, http.StatusCreated, q)
}

func (s *Server) handleQueryDelete(w http.ResponseWriter, r *http.Request) {
	u := currentUser(r)
	id := r.PathValue("id")
	ctx := r.Context()
	all, err := s.store.Queries(ctx)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	var found *store.SavedQuery
	for i := range all {
		if all[i].ID == id {
			found = &all[i]
			break
		}
	}
	if found == nil || (found.Owner != u.Email && u.Role != auth.RoleAdmin) {
		writeError(w, http.StatusNotFound, "not_found", "Consulta salva não encontrada.", nil)
		return
	}
	if err := s.store.DeleteQuery(ctx, id); err != nil {
		s.fail(w, r, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// ---------- checkpoints e verificação ----------

var checkpointName = regexp.MustCompile(`^[A-Za-z0-9._-]{1,128}$`)

func (s *Server) handleCheckpoints(w http.ResponseWriter, r *http.Request) {
	v, err := s.client.Check(r.Context(), "ARCA.CHECKPOINTS")
	if err != nil {
		s.fail(w, r, err)
		return
	}
	text := strings.TrimSpace(v.Text())
	if v.IsNull() || text == "" || text == "null" {
		writeRaw(w, http.StatusOK, []byte("[]\n"))
		return
	}
	if !json.Valid([]byte(text)) {
		s.fail(w, r, errors.New("lista de checkpoints inválida"))
		return
	}
	writeRaw(w, http.StatusOK, []byte(text))
}

func notFoundText(msg string) bool {
	m := strings.ToLower(msg)
	for _, s := range []string{"não existe", "nao existe", "não encontrado", "nao encontrado", "not found", "no such"} {
		if strings.Contains(m, s) {
			return true
		}
	}
	return false
}

func (s *Server) handleCheckpointDelete(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !checkpointName.MatchString(name) || name == "." || name == ".." {
		writeError(w, http.StatusBadRequest, "validation", "Nome de checkpoint inválido.", map[string]string{"name": "use letras, números, ponto, hífen e sublinhado"})
		return
	}
	v, err := s.client.DoTimeout(r.Context(), 2*time.Minute, "ARCA.CHECKPOINTS", "DELETE", name)
	if err == nil {
		err = v.Err()
	}
	if err != nil {
		var serverErr *resp.ServerError
		if errors.As(err, &serverErr) && notFoundText(serverErr.Text()) {
			writeError(w, http.StatusNotFound, "not_found", "Checkpoint não encontrado.", nil)
			return
		}
		s.fail(w, r, err)
		return
	}
	s.audit(r, "checkpoint_removido", name)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleVerify(w http.ResponseWriter, r *http.Request) {
	v, err := s.client.DoTimeout(r.Context(), 10*time.Minute, "ARCA.VERIFY")
	if err == nil {
		err = v.Err()
	}
	if err != nil {
		s.fail(w, r, err)
		return
	}
	raw := []byte(strings.TrimSpace(v.Text()))
	var res struct {
		Files   int64 `json:"files"`
		Corrupt []any `json:"corrupt"`
	}
	if err := json.Unmarshal(raw, &res); err != nil {
		s.fail(w, r, fmt.Errorf("resposta de ARCA.VERIFY inválida: %w", err))
		return
	}
	detail := fmt.Sprintf("%d arquivos conferidos", res.Files)
	if len(res.Corrupt) > 0 {
		detail += fmt.Sprintf(", %d com problema", len(res.Corrupt))
	}
	s.audit(r, "verificacao", detail)
	writeRaw(w, http.StatusOK, raw)
}

// ---------- integração ----------

func (s *Server) handleIntegrations(w http.ResponseWriter, r *http.Request) {
	addr := s.cfg.PublicAddr
	if addr == "" {
		addr = s.cfg.ArcaAddr
	}
	if addr == "" {
		addr = "127.0.0.1:7379"
	}
	host, portText, err := net.SplitHostPort(addr)
	if err != nil {
		host, portText = addr, "7379"
	}
	if host == "" || host == "0.0.0.0" || host == "::" {
		host = "localhost"
	}
	port, _ := strconv.Atoi(portText)
	scheme := "http"
	if r.TLS != nil || strings.EqualFold(r.Header.Get("X-Forwarded-Proto"), "https") {
		scheme = "https"
	}
	httpHost := r.Host
	if fh := r.Header.Get("X-Forwarded-Host"); fh != "" {
		httpHost = fh
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"version":           Version,
		"redis_host":        host,
		"redis_port":        port,
		"http_base":         scheme + "://" + httpHost,
		"openapi_url":       "/api/openapi.json",
		"metrics_url":       "/metrics",
		"requires_password": s.cfg.ArcaPassword != "",
	})
}

// ---------- Prometheus ----------

type promWriter struct {
	b strings.Builder
}

func (p *promWriter) family(name, typ, help string) {
	fmt.Fprintf(&p.b, "# HELP %s %s\n# TYPE %s %s\n", name, help, name, typ)
}

func promValue(v float64) string {
	return strconv.FormatFloat(v, 'g', -1, 64)
}

func (p *promWriter) sample(name, labels string, v float64) {
	if labels != "" {
		fmt.Fprintf(&p.b, "%s{%s} %s\n", name, labels, promValue(v))
		return
	}
	fmt.Fprintf(&p.b, "%s %s\n", name, promValue(v))
}

func (p *promWriter) metric(name, typ, help string, v float64) {
	p.family(name, typ, help)
	p.sample(name, "", v)
}

type promStats struct {
	Server struct {
		UptimeS float64 `json:"uptime_s"`
	} `json:"server"`
	Clients struct {
		Connected float64 `json:"connected"`
		Total     float64 `json:"total"`
	} `json:"clients"`
	Commands struct {
		Total          float64 `json:"total"`
		OpsPerSec      float64 `json:"ops_per_sec"`
		KeyspaceHits   float64 `json:"keyspace_hits"`
		KeyspaceMisses float64 `json:"keyspace_misses"`
		ExpiredKeys    float64 `json:"expired_keys"`
		NetInput       float64 `json:"net_input_bytes"`
		NetOutput      float64 `json:"net_output_bytes"`
	} `json:"commands"`
	Engine struct {
		MemtableBytes          float64 `json:"memtable_bytes"`
		ImmutableBytes         float64 `json:"immutable_bytes"`
		WALBytes               float64 `json:"wal_bytes"`
		Compactions            float64 `json:"compactions"`
		CompactionBytesRead    float64 `json:"compaction_bytes_read"`
		CompactionBytesWritten float64 `json:"compaction_bytes_written"`
		WriteAmplification     float64 `json:"write_amplification"`
		BlockCacheHits         float64 `json:"block_cache_hits"`
		BlockCacheMisses       float64 `json:"block_cache_misses"`
		EstimatedKeys          float64 `json:"estimated_keys"`
	} `json:"engine"`
	Levels []struct {
		Level int     `json:"level"`
		Files float64 `json:"files"`
		Bytes float64 `json:"bytes"`
	} `json:"levels"`
}

func (s *Server) handleMetrics(w http.ResponseWriter, r *http.Request) {
	var p promWriter
	raw, err := s.store.Stats(r.Context())
	var st promStats
	if err == nil {
		err = json.Unmarshal(raw, &st)
	}
	if err != nil {
		p.metric("arca_up", "gauge", "1 se o servidor Arca respondeu à coleta.", 0)
	} else {
		p.metric("arca_up", "gauge", "1 se o servidor Arca respondeu à coleta.", 1)
		p.metric("arca_uptime_seconds", "gauge", "Segundos desde que o servidor subiu.", st.Server.UptimeS)
		p.metric("arca_ops_per_second", "gauge", "Comandos por segundo no último segundo.", st.Commands.OpsPerSec)
		p.metric("arca_commands_processed_total", "counter", "Comandos processados desde a partida.", st.Commands.Total)
		p.metric("arca_connected_clients", "gauge", "Conexões abertas no servidor.", st.Clients.Connected)
		p.metric("arca_memtable_bytes", "gauge", "Bytes na memtable atual.", st.Engine.MemtableBytes)
		p.metric("arca_immutable_memtable_bytes", "gauge", "Bytes na memtable imutável esperando o flush.", st.Engine.ImmutableBytes)
		p.metric("arca_wal_bytes", "gauge", "Tamanho do log de escrita atual.", st.Engine.WALBytes)
		p.family("arca_sst_files", "gauge", "SSTables por nível da LSM-Tree.")
		for _, l := range st.Levels {
			p.sample("arca_sst_files", fmt.Sprintf(`level="%d"`, l.Level), l.Files)
		}
		p.family("arca_sst_bytes", "gauge", "Bytes em SSTables por nível da LSM-Tree.")
		for _, l := range st.Levels {
			p.sample("arca_sst_bytes", fmt.Sprintf(`level="%d"`, l.Level), l.Bytes)
		}
		p.metric("arca_compactions_total", "counter", "Compactações concluídas.", st.Engine.Compactions)
		p.metric("arca_compaction_bytes_read_total", "counter", "Bytes lidos pelas compactações.", st.Engine.CompactionBytesRead)
		p.metric("arca_compaction_bytes_written_total", "counter", "Bytes gravados pelas compactações.", st.Engine.CompactionBytesWritten)
		p.metric("arca_write_amplification", "gauge", "Bytes gravados em disco por byte escrito pelo usuário.", st.Engine.WriteAmplification)
		ratio := 0.0
		if t := st.Engine.BlockCacheHits + st.Engine.BlockCacheMisses; t > 0 {
			ratio = st.Engine.BlockCacheHits / t
		}
		p.metric("arca_block_cache_hit_ratio", "gauge", "Fração de leituras de bloco atendidas pelo cache desde a partida.", ratio)
		p.metric("arca_keyspace_hits_total", "counter", "Leituras que encontraram a chave.", st.Commands.KeyspaceHits)
		p.metric("arca_keyspace_misses_total", "counter", "Leituras que não encontraram a chave.", st.Commands.KeyspaceMisses)
		p.metric("arca_expired_keys_total", "counter", "Chaves removidas por TTL.", st.Commands.ExpiredKeys)
		p.metric("arca_net_input_bytes_total", "counter", "Bytes recebidos pela rede.", st.Commands.NetInput)
		p.metric("arca_net_output_bytes_total", "counter", "Bytes enviados pela rede.", st.Commands.NetOutput)
		p.metric("arca_estimated_keys", "gauge", "Estimativa de chaves e linhas guardadas.", st.Engine.EstimatedKeys)
	}
	p.family("arca_gateway_http_requests_total", "counter", "Requisições à API do gateway por código de resposta.")
	counts := s.httpSnapshot()
	codes := make([]int, 0, len(counts))
	for c := range counts {
		codes = append(codes, c)
	}
	sort.Ints(codes)
	for _, c := range codes {
		p.sample("arca_gateway_http_requests_total", fmt.Sprintf(`code="%d"`, c), float64(counts[c]))
	}
	p.metric("arca_gateway_sse_clients", "gauge", "Painéis conectados ao fluxo de métricas.", float64(s.hub.Subscribers()))
	w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	w.Write([]byte(p.b.String()))
}

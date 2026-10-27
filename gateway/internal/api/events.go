package api

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"arca/gateway/internal/store"
)

type Metrics struct {
	T                  int64   `json:"t"`
	OpsPerSec          float64 `json:"ops_per_sec"`
	ReadsPerSec        float64 `json:"reads_per_sec"`
	WritesPerSec       float64 `json:"writes_per_sec"`
	MemtableBytes      int64   `json:"memtable_bytes"`
	ImmutableBytes     int64   `json:"immutable_bytes"`
	SSTBytes           int64   `json:"sst_bytes"`
	SSTFiles           int64   `json:"sst_files"`
	Clients            int64   `json:"clients"`
	CacheHitRatio      float64 `json:"cache_hit_ratio"`
	Compacting         bool    `json:"compacting"`
	LevelFiles         []int64 `json:"level_files"`
	WriteAmplification float64 `json:"write_amplification"`
}

type statsSample struct {
	Clients struct {
		Connected int64 `json:"connected"`
	} `json:"clients"`
	Commands struct {
		OpsPerSec float64 `json:"ops_per_sec"`
		Reads     int64   `json:"reads"`
		Writes    int64   `json:"writes"`
	} `json:"commands"`
	Engine struct {
		MemtableBytes      int64   `json:"memtable_bytes"`
		ImmutableBytes     int64   `json:"immutable_bytes"`
		SSTFiles           int64   `json:"sst_files"`
		SSTBytes           int64   `json:"sst_bytes"`
		BlockCacheHits     int64   `json:"block_cache_hits"`
		BlockCacheMisses   int64   `json:"block_cache_misses"`
		Compacting         bool    `json:"compacting"`
		WriteAmplification float64 `json:"write_amplification"`
	} `json:"engine"`
	Levels []struct {
		Level int   `json:"level"`
		Files int64 `json:"files"`
	} `json:"levels"`
}

type Hub struct {
	store    *store.Store
	log      *slog.Logger
	interval time.Duration
	mu       sync.Mutex
	subs     map[chan []byte]struct{}
	cancel   context.CancelFunc
	last     []byte
}

func NewHub(st *store.Store, logger *slog.Logger, interval time.Duration) *Hub {
	return &Hub{store: st, log: logger, interval: interval, subs: map[chan []byte]struct{}{}}
}

func (h *Hub) Subscribe() (<-chan []byte, func()) {
	ch := make(chan []byte, 4)
	h.mu.Lock()
	h.subs[ch] = struct{}{}
	if h.cancel == nil {
		ctx, cancel := context.WithCancel(context.Background())
		h.cancel = cancel
		go h.run(ctx)
	}
	if h.last != nil {
		ch <- h.last
	}
	h.mu.Unlock()
	return ch, func() {
		h.mu.Lock()
		if _, ok := h.subs[ch]; ok {
			delete(h.subs, ch)
			close(ch)
		}
		if len(h.subs) == 0 && h.cancel != nil {
			h.cancel()
			h.cancel = nil
			h.last = nil
		}
		h.mu.Unlock()
	}
}

func (h *Hub) Subscribers() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.subs)
}

func (h *Hub) broadcast(frame []byte) {
	h.mu.Lock()
	h.last = frame
	for ch := range h.subs {
		select {
		case ch <- frame:
		default:
		}
	}
	h.mu.Unlock()
}

func (h *Hub) run(ctx context.Context) {
	ticker := time.NewTicker(h.interval)
	defer ticker.Stop()
	var prev *statsSample
	var prevAt time.Time
	for {
		sample, err := h.sample(ctx)
		now := time.Now()
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			payload, _ := json.Marshal(map[string]any{"t": now.UnixMilli(), "message": "O Arca não está respondendo."})
			h.broadcast(frame("unavailable", payload))
			prev = nil
		} else {
			m := computeMetrics(prev, sample, now.Sub(prevAt))
			m.T = now.UnixMilli()
			payload, _ := json.Marshal(m)
			h.broadcast(frame("metrics", payload))
			prev = sample
			prevAt = now
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (h *Hub) sample(ctx context.Context) (*statsSample, error) {
	cctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	raw, err := h.store.Stats(cctx)
	if err != nil {
		return nil, err
	}
	var s statsSample
	if err := json.Unmarshal(raw, &s); err != nil {
		return nil, err
	}
	return &s, nil
}

func computeMetrics(prev, cur *statsSample, elapsed time.Duration) Metrics {
	m := Metrics{
		OpsPerSec:          cur.Commands.OpsPerSec,
		MemtableBytes:      cur.Engine.MemtableBytes,
		ImmutableBytes:     cur.Engine.ImmutableBytes,
		SSTBytes:           cur.Engine.SSTBytes,
		SSTFiles:           cur.Engine.SSTFiles,
		Clients:            cur.Clients.Connected,
		Compacting:         cur.Engine.Compacting,
		WriteAmplification: cur.Engine.WriteAmplification,
		LevelFiles:         make([]int64, 7),
	}
	for _, l := range cur.Levels {
		if l.Level >= 0 && l.Level < 7 {
			m.LevelFiles[l.Level] = l.Files
		}
	}
	hits, misses := cur.Engine.BlockCacheHits, cur.Engine.BlockCacheMisses
	if prev != nil && elapsed > 0 {
		secs := elapsed.Seconds()
		m.ReadsPerSec = clampRate(float64(cur.Commands.Reads-prev.Commands.Reads) / secs)
		m.WritesPerSec = clampRate(float64(cur.Commands.Writes-prev.Commands.Writes) / secs)
		dh, dm := hits-prev.Engine.BlockCacheHits, misses-prev.Engine.BlockCacheMisses
		if dh >= 0 && dm >= 0 && dh+dm > 0 {
			hits, misses = dh, dm
		}
	}
	if hits+misses > 0 {
		m.CacheHitRatio = float64(hits) / float64(hits+misses)
	}
	return m
}

func clampRate(v float64) float64 {
	if v < 0 {
		return 0
	}
	return v
}

func frame(event string, payload []byte) []byte {
	return []byte(fmt.Sprintf("event: %s\ndata: %s\n\n", event, payload))
}

func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request) {
	token := bearerToken(r)
	if token == "" {
		token = r.URL.Query().Get("token")
	}
	u, status, msg := s.authenticate(r, token)
	if u == nil {
		writeError(w, status, "unauthorized", msg, nil)
		return
	}
	flusher, ok := w.(http.Flusher)
	if !ok {
		writeError(w, http.StatusInternalServerError, "internal", "Streaming não suportado.", nil)
		return
	}
	h := w.Header()
	h.Set("Content-Type", "text/event-stream; charset=utf-8")
	h.Set("Cache-Control", "no-store")
	h.Set("Connection", "keep-alive")
	h.Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, "retry: 3000\n\n")
	flusher.Flush()
	ch, unsubscribe := s.hub.Subscribe()
	defer unsubscribe()
	keepalive := time.NewTicker(20 * time.Second)
	defer keepalive.Stop()
	expiry := time.NewTicker(time.Minute)
	defer expiry.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case data, ok := <-ch:
			if !ok {
				return
			}
			if _, err := w.Write(data); err != nil {
				return
			}
			flusher.Flush()
		case <-keepalive.C:
			if _, err := fmt.Fprint(w, "event: ping\ndata: {}\n\n"); err != nil {
				return
			}
			flusher.Flush()
		case <-expiry.C:
			if u, status, _ := s.authenticate(r, token); u == nil && status == http.StatusUnauthorized {
				fmt.Fprint(w, "event: expired\ndata: {}\n\n")
				flusher.Flush()
				return
			}
		}
	}
}

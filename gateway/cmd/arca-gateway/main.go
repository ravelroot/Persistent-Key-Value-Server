package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"arca/gateway/internal/api"
	"arca/gateway/internal/config"
	"arca/gateway/internal/resp"
	"arca/gateway/internal/seed"
	"arca/gateway/internal/store"
)

func main() {
	cfg := config.FromEnv()
	level := slog.LevelInfo
	switch cfg.LogLevel {
	case "debug":
		level = slog.LevelDebug
	case "warn":
		level = slog.LevelWarn
	case "error":
		level = slog.LevelError
	}
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: level}))

	client := resp.NewClient(resp.Options{Addr: cfg.ArcaAddr, Password: cfg.ArcaPassword, PoolSize: cfg.PoolSize})
	defer client.Close()
	st := store.New(client)
	srv := api.New(cfg, client, st, logger)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go prepare(ctx, cfg, client, st, srv, logger)

	httpServer := &http.Server{
		Addr:              cfg.HTTPAddr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       60 * time.Second,
		IdleTimeout:       120 * time.Second,
		MaxHeaderBytes:    64 << 10,
	}
	errCh := make(chan error, 1)
	go func() {
		logger.Info("gateway no ar", "versao", api.Version, "http", cfg.HTTPAddr, "arca", cfg.ArcaAddr, "painel", cfg.WebDir)
		errCh <- httpServer.ListenAndServe()
	}()

	select {
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("falha no servidor HTTP", "erro", err)
			os.Exit(1)
		}
	case <-ctx.Done():
		logger.Info("encerrando o gateway")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		httpServer.Shutdown(shutdownCtx)
	}
}

func prepare(ctx context.Context, cfg config.Config, client *resp.Client, st *store.Store, srv *api.Server, logger *slog.Logger) {
	wait := 500 * time.Millisecond
	attempt := 0
	for {
		attempt++
		err := client.Ping(ctx)
		if err == nil {
			err = st.Bootstrap(ctx)
		}
		if err == nil {
			break
		}
		if ctx.Err() != nil {
			return
		}
		if attempt == 1 || attempt%10 == 0 {
			logger.Warn("aguardando o Arca", "endereco", cfg.ArcaAddr, "tentativa", attempt, "erro", err)
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(wait):
		}
		if wait < 5*time.Second {
			wait *= 2
		}
	}
	if cfg.Seed {
		seeder := &seed.Seeder{Store: st, Now: time.Now}
		seedCtx, cancel := context.WithTimeout(ctx, 5*time.Minute)
		created, err := seeder.Run(seedCtx)
		cancel()
		switch {
		case err != nil:
			logger.Error("falha ao criar os dados de demonstração", "erro", err)
		case created:
			logger.Info("contas e dados de demonstração criados", "senha", seed.DemoPassword)
		}
	}
	srv.SetReady(true)
	logger.Info("gateway pronto")
}

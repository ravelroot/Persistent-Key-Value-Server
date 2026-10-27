package config

import (
	"os"
	"strconv"
	"strings"
	"time"
)

type Config struct {
	ArcaAddr     string
	PublicAddr   string
	ArcaPassword string
	HTTPAddr     string
	WebDir       string
	Seed         bool
	SessionTTL   time.Duration
	PoolSize     int
	LogLevel     string
}

func FromEnv() Config {
	return Config{
		ArcaAddr:     env("ARCA_ADDR", "127.0.0.1:7379"),
		PublicAddr:   os.Getenv("ARCA_PUBLIC_ADDR"),
		ArcaPassword: os.Getenv("ARCA_PASSWORD"),
		HTTPAddr:     env("ARCA_HTTP_ADDR", ":8080"),
		WebDir:       env("ARCA_WEB_DIR", "../web/dist"),
		Seed:         envBool("ARCA_SEED", true),
		SessionTTL:   time.Duration(envInt("ARCA_SESSION_HOURS", 12, 1, 24*30)) * time.Hour,
		PoolSize:     envInt("ARCA_POOL_SIZE", 16, 1, 512),
		LogLevel:     strings.ToLower(env("ARCA_LOG_LEVEL", "info")),
	}
}

func env(name, fallback string) string {
	if v := strings.TrimSpace(os.Getenv(name)); v != "" {
		return v
	}
	return fallback
}

func envBool(name string, fallback bool) bool {
	v := strings.ToLower(strings.TrimSpace(os.Getenv(name)))
	switch v {
	case "1", "true", "yes", "sim", "on":
		return true
	case "0", "false", "no", "nao", "não", "off":
		return false
	}
	return fallback
}

func envInt(name string, fallback, min, max int) int {
	v, err := strconv.Atoi(strings.TrimSpace(os.Getenv(name)))
	if err != nil || v < min || v > max {
		return fallback
	}
	return v
}

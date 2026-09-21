// migrate applies (or rolls back) the SQL files in api/migrations.
//
//	go run ./cmd/migrate up
//	go run ./cmd/migrate down 1
//	go run ./cmd/migrate status
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"strconv"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/matthewlee/benchmark-platform/api/internal/config"
	"github.com/matthewlee/benchmark-platform/api/migrations"
)

func main() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: migrate [up|down N|status]\n")
	}
	flag.Parse()
	cmd := flag.Arg(0)
	if cmd == "" {
		cmd = "up"
	}

	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	cfg := config.Load()
	conn, err := pgx.Connect(ctx, cfg.DatabaseURL)
	if err != nil {
		fatal("connect: %v", err)
	}
	defer conn.Close(ctx)

	switch cmd {
	case "up":
		done, err := migrations.Up(ctx, conn)
		if err != nil {
			fatal("%v", err)
		}
		if len(done) == 0 {
			fmt.Println("migrate: already up to date")
			return
		}
		fmt.Printf("migrate: applied %v\n", done)
	case "down":
		n := 1
		if arg := flag.Arg(1); arg != "" {
			if parsed, err := strconv.Atoi(arg); err == nil {
				n = parsed
			}
		}
		done, err := migrations.Down(ctx, conn, n)
		if err != nil {
			fatal("%v", err)
		}
		fmt.Printf("migrate: rolled back %v\n", done)
	case "status":
		ms, err := migrations.Load()
		if err != nil {
			fatal("%v", err)
		}
		applied, err := migrations.Applied(ctx, conn)
		if err != nil {
			fatal("%v", err)
		}
		for _, m := range ms {
			state := "pending"
			if applied[m.Version] {
				state = "applied"
			}
			fmt.Printf("%04d_%s  %s\n", m.Version, m.Name, state)
		}
	default:
		flag.Usage()
		os.Exit(2)
	}
}

func fatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "migrate: "+format+"\n", args...)
	os.Exit(1)
}

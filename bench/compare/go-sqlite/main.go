// go-sqlite — the comparison harness for docs/examples/db-bench.
// Mirrors the .wo sample's schema and modes so the lines align
// column-for-column: <op> <count> <ops/sec> <p50us> <p99us>.
//
// Flavors mirror the campaign's: "ram" = :memory:, "durable" = a file
// with synchronous=FULL and per-statement autocommit — an fsync per
// insert, the same ack-after-durable contract writeonce's WAL gives.
//
// Usage: go-sqlite <ram|durable> <N> [dir]
package main

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

func pct(d []time.Duration, p int) int64 {
	if len(d) == 0 {
		return 0
	}
	s := make([]time.Duration, len(d))
	copy(s, d)
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	i := len(s) * p / 100
	if i >= len(s) {
		i = len(s) - 1
	}
	return s[i].Microseconds()
}

func report(op string, n int, total time.Duration, per []time.Duration) {
	us := total.Microseconds()
	if us < 1 {
		us = 1
	}
	fmt.Printf("%s %d %d %d %d\n", op, n, int64(n)*1e6/us, pct(per, 50), pct(per, 99))
}

func must(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, "go-sqlite:", err)
		os.Exit(1)
	}
}

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: go-sqlite <ram|durable> <N> [dir]")
		os.Exit(2)
	}
	flavor := os.Args[1]
	var n int
	fmt.Sscanf(os.Args[2], "%d", &n)
	dsn := ":memory:"
	if flavor == "durable" {
		dir := "."
		if len(os.Args) > 3 {
			dir = os.Args[3]
		}
		// FULL = fsync before every commit acknowledges — the peer of
		// writeonce's per-statement WAL commit
		dsn = filepath.Join(dir, "bench.db") + "?_journal_mode=WAL&_synchronous=FULL"
	}
	db, err := sql.Open("sqlite3", dsn)
	must(err)
	defer db.Close()
	db.SetMaxOpenConns(1) // one writer, like the engine; keeps :memory: coherent

	_, err = db.Exec(`
	  CREATE TABLE buckets (id INTEGER PRIMARY KEY, tag TEXT NOT NULL UNIQUE);
	  CREATE TABLE items (id INTEGER PRIMARY KEY, k INTEGER NOT NULL,
	                      v INTEGER NOT NULL,
	                      bucket INTEGER NOT NULL REFERENCES buckets(id));
	  CREATE INDEX items_k ON items(k);
	  CREATE INDEX items_bucket ON items(bucket);
	  PRAGMA foreign_keys = ON;`)
	must(err)

	kmod := n / 10
	if kmod < 1 {
		kmod = 1
	}
	itemV := func(i int) int { return (i * 37) % 1000 }
	lcg := func(s int) int {
		x := s*1103515245 + 12345
		if x < 0 {
			x = -x
		}
		return x
	}

	// seed: one bucket per 100 children, per-statement autocommit —
	// mirror of the .wo sample's ack-per-insert shape
	insB, err := db.Prepare("INSERT INTO buckets(tag) VALUES(?)")
	must(err)
	insI, err := db.Prepare("INSERT INTO items(k, v, bucket) VALUES(?, ?, ?)")
	must(err)
	per := make([]time.Duration, 0, n)
	t0 := time.Now()
	var bref int64
	for i, b := 1, 0; i <= n; b++ {
		r, err := insB.Exec(fmt.Sprintf("b%d", b))
		must(err)
		bref, _ = r.LastInsertId()
		for j := 0; j < 100 && i <= n; j, i = j+1, i+1 {
			o0 := time.Now()
			_, err = insI.Exec(i%kmod, itemV(i), bref)
			must(err)
			per = append(per, time.Since(o0))
		}
	}
	report("seed", n, time.Since(t0), per)

	// read: indexed point lookups, LIMIT 1 — the .wo take-1 shape
	rd, err := db.Prepare("SELECT v FROM items WHERE k = ? LIMIT 1")
	must(err)
	per = per[:0]
	sink, s := 0, 42
	nr := n / 2
	t0 = time.Now()
	for i := 0; i < nr; i++ {
		s = lcg(s)
		o0 := time.Now()
		var v int
		if err := rd.QueryRow(s % kmod).Scan(&v); err == nil {
			sink += v
		}
		per = append(per, time.Since(o0))
	}
	report("read", nr, time.Since(t0), per)

	// query: full equality probes (~10 rows each), materialized + counted
	qr, err := db.Prepare("SELECT v FROM items WHERE k = ?")
	must(err)
	per = per[:0]
	rows, s := 0, 7
	nq := n / 10
	t0 = time.Now()
	for i := 0; i < nq; i++ {
		s = lcg(s)
		o0 := time.Now()
		rs, err := qr.Query(s % kmod)
		must(err)
		for rs.Next() {
			rows++
		}
		rs.Close()
		per = append(per, time.Since(o0))
	}
	report("query", nq, time.Since(t0), per)
	fmt.Printf("query rows %d\n", rows)

	// write: alternating inserts (disjoint k) and update-through-query
	up, err := db.Prepare(
		"UPDATE items SET v = v + 1 WHERE id = (SELECT id FROM items WHERE k = ? LIMIT 1)")
	must(err)
	per = per[:0]
	s = 99
	nw := n / 2
	t0 = time.Now()
	for i := 0; i < nw; i++ {
		o0 := time.Now()
		if i%2 == 0 {
			_, err = insI.Exec(2000000+i, itemV(i), bref)
		} else {
			s = lcg(s)
			_, err = up.Exec(s % kmod)
		}
		must(err)
		per = append(per, time.Since(o0))
	}
	report("write", nw, time.Since(t0), per)
	_ = sink
}

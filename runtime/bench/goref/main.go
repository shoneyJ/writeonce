// goref — the Go net/http comparison server for the phase-F benchmark.
// Same endpoints and semantics as wo-rt-c's RAM read path: /healthz, /,
// GET/POST /api/notes against an in-memory store. Durability is NOT
// implemented here (Go side has no WAL), so only READ benchmarks are
// apples-to-apples; the POST comparison measures Go's non-durable path
// against wo-rt-c's fsync-backed path and is labeled as such.
//
//	go build -o goref . && ./goref           # :8095, GOMAXPROCS = all cores
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sync"
)

type note struct {
	ID    int    `json:"id"`
	Title string `json:"title"`
}

var (
	mu     sync.RWMutex
	notes  []note
	nextID = 1
)

func main() {
	http.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "ok")
	})
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, `{"runtime":"go-net-http"}`)
	})
	http.HandleFunc("/api/notes", func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet:
			mu.RLock()
			b, _ := json.Marshal(notes)
			mu.RUnlock()
			w.Header().Set("Content-Type", "application/json")
			w.Write(b)
		case http.MethodPost:
			var in struct {
				Title string `json:"title"`
			}
			if json.NewDecoder(r.Body).Decode(&in) != nil || in.Title == "" {
				http.Error(w, `{"error":"expected {\"title\":\"...\"}"}`, http.StatusBadRequest)
				return
			}
			mu.Lock()
			n := note{ID: nextID, Title: in.Title}
			nextID++
			if len(notes) < 100000 {
				notes = append(notes, n)
			}
			mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			json.NewEncoder(w).Encode(n)
		default:
			http.Error(w, "method", http.StatusMethodNotAllowed)
		}
	})
	fmt.Println("[goref] listening on :8095")
	http.ListenAndServe("127.0.0.1:8095", nil)
}

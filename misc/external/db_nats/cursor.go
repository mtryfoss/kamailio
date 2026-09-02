package main

import (
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"fmt"
	"sync"
	"time"
)

// Cursor holds an open result set between fetch_result() calls.
//
// The srdb1 API has no guaranteed "close cursor" call - db_nats closes one on
// free_result, but a crashing or wedged client would otherwise pin a database
// connection forever. Hence both an idle TTL and a hard cap.
type Cursor struct {
	ID       string
	rows     *sql.Rows
	cols     []Col
	lastUsed time.Time
}

type CursorStore struct {
	mu   sync.Mutex
	m    map[string]*Cursor
	max  int
	ttl  time.Duration
	stop chan struct{}
}

func NewCursorStore(max int, ttl time.Duration) *CursorStore {
	cs := &CursorStore{
		m:    make(map[string]*Cursor),
		max:  max,
		ttl:  ttl,
		stop: make(chan struct{}),
	}
	go cs.reaper()
	return cs
}

func (cs *CursorStore) Add(rows *sql.Rows, cols []Col) (*Cursor, error) {
	cs.mu.Lock()
	defer cs.mu.Unlock()

	if len(cs.m) >= cs.max {
		return nil, fmt.Errorf("too many open cursors (max %d)", cs.max)
	}

	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return nil, err
	}
	c := &Cursor{
		ID:       hex.EncodeToString(b[:]),
		rows:     rows,
		cols:     cols,
		lastUsed: time.Now(),
	}
	cs.m[c.ID] = c
	return c, nil
}

func (cs *CursorStore) Get(id string) (*Cursor, bool) {
	cs.mu.Lock()
	defer cs.mu.Unlock()
	c, ok := cs.m[id]
	if ok {
		c.lastUsed = time.Now()
	}
	return c, ok
}

// Len reports how many cursors are still open, so shutdown can wait for
// clients to finish paging rather than pulling result sets out from under
// them.
func (cs *CursorStore) Len() int {
	cs.mu.Lock()
	defer cs.mu.Unlock()
	return len(cs.m)
}

func (cs *CursorStore) Close(id string) {
	cs.mu.Lock()
	c, ok := cs.m[id]
	if ok {
		delete(cs.m, id)
	}
	cs.mu.Unlock()

	if ok {
		c.rows.Close()
	}
}

func (cs *CursorStore) Shutdown() {
	close(cs.stop)
	cs.mu.Lock()
	defer cs.mu.Unlock()
	for id, c := range cs.m {
		c.rows.Close()
		delete(cs.m, id)
	}
}

func (cs *CursorStore) reaper() {
	t := time.NewTicker(cs.ttl / 2)
	defer t.Stop()
	for {
		select {
		case <-cs.stop:
			return
		case <-t.C:
			cutoff := time.Now().Add(-cs.ttl)
			cs.mu.Lock()
			for id, c := range cs.m {
				if c.lastUsed.Before(cutoff) {
					c.rows.Close()
					delete(cs.m, id)
				}
			}
			cs.mu.Unlock()
		}
	}
}

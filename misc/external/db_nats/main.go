// Command db_nats_backend terminates Kamailio db_nats requests against MySQL.
//
// It subscribes to three subjects under a configurable prefix:
//
//	<prefix>.op      structured operations (select/insert/update/delete/...)
//	<prefix>.raw     verbatim SQL passthrough, disabled by default
//	<prefix>.cursor  fetch continuation and cursor close
//
// Subjects are split so that NATS account permissions can deny the raw path
// per credential, keeping enforcement on the bus rather than in this process.
package main

import (
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	_ "time/tzdata" // embedded zoneinfo for minimal container images

	_ "github.com/go-sql-driver/mysql"
	"github.com/nats-io/nats.go"
)

func main() {
	cfgPath := flag.String("config", "config.yaml", "path to the YAML configuration")
	flag.Parse()

	cfg, err := LoadConfig(*cfgPath)
	if err != nil {
		log.Fatalf("config: %v", err)
	}

	db, err := sql.Open("mysql", cfg.MySQL.DSN)
	if err != nil {
		log.Fatalf("mysql: %v", err)
	}
	defer db.Close()

	db.SetMaxOpenConns(cfg.MySQL.MaxOpenConns)
	db.SetMaxIdleConns(cfg.MySQL.MaxIdleConns)
	db.SetConnMaxLifetime(5 * time.Minute)

	if err := db.Ping(); err != nil {
		log.Fatalf("mysql ping: %v", err)
	}

	opts := []nats.Option{
		nats.Name("kamailio-db_nats-backend"),
		nats.MaxReconnects(-1),
		nats.ReconnectWait(2 * time.Second),
		nats.DisconnectErrHandler(func(_ *nats.Conn, err error) {
			log.Printf("nats disconnected: %v", err)
		}),
		nats.ReconnectHandler(func(nc *nats.Conn) {
			log.Printf("nats reconnected to %s", nc.ConnectedUrl())
		}),
	}
	if cfg.NATS.CredsFile != "" {
		opts = append(opts, nats.UserCredentials(cfg.NATS.CredsFile))
	}

	nc, err := nats.Connect(cfg.NATS.URL, opts...)
	if err != nil {
		log.Fatalf("nats: %v", err)
	}
	defer nc.Drain()

	cursors := NewCursorStore(cfg.Cursors.Max, cfg.Cursors.IdleTTL)
	defer cursors.Shutdown()

	prefix := cfg.NATS.SubjectPrefix

	// Cursor state is process-local, so this instance needs an address of its
	// own for clients to send continuation to.
	instance, err := instanceID()
	if err != nil {
		log.Fatalf("instance id: %v", err)
	}
	cursorSubject := prefix + ".cursor." + instance

	h := NewHandler(db, cfg, cursors, cursorSubject)

	cursorFn := func(r *Request) Reply {
		switch r.Op {
		case "fetch":
			return h.Fetch(r)
		case "close":
			return h.CloseCursor(r)
		default:
			return errorReply(CodeBadRequest, "unknown cursor op "+r.Op)
		}
	}

	subs := []struct {
		subject string
		queue   bool
		fn      func(*Request) Reply
	}{
		// stateless: spread across the group
		{prefix + ".op", true, h.Handle},
		{prefix + ".raw", true, h.Raw},
		// kept queue-subscribed so a stray close from an older client, or one
		// whose owner has gone, is still absorbed rather than timing out
		{prefix + ".cursor", true, cursorFn},
		// stateful: addressed to this instance alone, never load-balanced
		{cursorSubject, false, cursorFn},
	}

	// in-flight handlers, so teardown never races a running scanPage
	var inflight sync.WaitGroup
	var queueSubs []*nats.Subscription
	var instanceSub *nats.Subscription

	for _, s := range subs {
		s := s
		cb := func(m *nats.Msg) {
			inflight.Add(1)
			defer inflight.Done()
			serve(m, s.fn, nc.MaxPayload())
		}
		var sub *nats.Subscription
		var err error
		if s.queue {
			sub, err = nc.QueueSubscribe(s.subject, cfg.NATS.QueueGroup, cb)
		} else {
			sub, err = nc.Subscribe(s.subject, cb)
		}
		if err != nil {
			log.Fatalf("subscribe %s: %v", s.subject, err)
		}
		if s.queue {
			queueSubs = append(queueSubs, sub)
		} else {
			instanceSub = sub
		}
		if s.queue {
			log.Printf("subscribed to %s (queue %q)", s.subject, cfg.NATS.QueueGroup)
		} else {
			log.Printf("subscribed to %s (this instance only)", s.subject)
		}
	}

	if err := nc.Flush(); err != nil {
		log.Fatalf("nats flush: %v", err)
	}
	log.Printf("ready: nats=%s mysql tz=%s raw=%v grants=%d",
		cfg.NATS.URL, cfg.MySQL.Timezone, cfg.NATS.EnableRaw, len(cfg.Grants))

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig

	// 1. Stop taking new work. These are queue subscriptions, so the group
	//    routes fresh requests to the remaining instances instead.
	log.Printf("shutting down: draining queue subscriptions")
	for _, sub := range queueSubs {
		if err := sub.Drain(); err != nil {
			log.Printf("drain %s: %v", sub.Subject, err)
		}
	}

	// 2. Keep the private cursor subject alive so clients already paging can
	//    finish. Without this a restart pulls the result set out from under a
	//    usrloc preload, which then fails.
	deadline := time.Now().Add(cfg.Shutdown.Grace)
	for cursors.Len() > 0 && time.Now().Before(deadline) {
		time.Sleep(200 * time.Millisecond)
	}
	if n := cursors.Len(); n > 0 {
		log.Printf("grace period expired with %d cursor(s) still open; "+
			"closing them now", n)
	} else {
		log.Printf("all cursors drained")
	}

	// 3. Now stop cursor traffic and let running handlers return before the
	//    deferred cursors.Shutdown() closes any sql.Rows they are reading.
	if instanceSub != nil {
		if err := instanceSub.Drain(); err != nil {
			log.Printf("drain %s: %v", instanceSub.Subject, err)
		}
	}
	inflight.Wait()
	log.Printf("shutdown complete")
}

// instanceID returns a short random id identifying this process.
func instanceID() (string, error) {
	var b [6]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(b[:]), nil
}

// serve decodes one request, runs it and always answers. A client blocked in
// natsConnection_Request() is holding a SIP worker, so every path must reply.
func serve(m *nats.Msg, fn func(*Request) Reply, maxPayload int64) {
	var req Request
	var rep Reply

	if err := json.Unmarshal(m.Data, &req); err != nil {
		rep = errorReply(CodeBadRequest, "malformed request: "+err.Error())
	} else {
		rep = safeCall(fn, &req)
	}

	b, err := json.Marshal(rep)
	if err != nil {
		b, _ = json.Marshal(errorReply(CodeBackendError, "cannot encode reply"))
	}

	// A reply over the server limit would be rejected by Respond, sending
	// nothing at all - the caller would then block for its whole timeout and
	// learn only that it timed out. Answer with an explanation instead.
	if maxPayload > 0 && int64(len(b)) > maxPayload {
		log.Printf("reply for op=%q table=%q is %d bytes, over the %d byte "+
			"server limit", req.Op, req.Table, len(b), maxPayload)
		b, _ = json.Marshal(errorReply(CodeReplyTooLarge, fmt.Sprintf(
			"reply is %d bytes, over the server max_payload of %d; "+
				"use a driver that pages with fetch_result, narrow the "+
				"query, or raise max_payload", len(b), maxPayload)))
	}

	if err := m.Respond(b); err != nil {
		log.Printf("respond on %s: %v", m.Subject, err)
	}
}

// safeCall keeps one bad request from taking the process down mid-reply.
func safeCall(fn func(*Request) Reply, req *Request) (rep Reply) {
	defer func() {
		if r := recover(); r != nil {
			log.Printf("panic handling op=%q table=%q: %v", req.Op, req.Table, r)
			rep = errorReply(CodeBackendError, "internal error")
		}
	}()
	return fn(req)
}

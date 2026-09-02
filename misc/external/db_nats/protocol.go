package main

import "encoding/base64"

// ProtoVersion must match DB_NATS_PROTO_VER in db_nats_mod.h.
const ProtoVersion = 1

// Value is one typed datum. D always carries the datum as a string, never as a
// JSON number: the C side parses with srjson, which stores every number as a
// double and would silently corrupt integers above 2^53.
type Value struct {
	T string `json:"t"`
	D string `json:"d,omitempty"`
	N bool   `json:"n,omitempty"` // SQL NULL
}

// Term is one WHERE condition.
type Term struct {
	C string `json:"c"`
	O string `json:"o"`
	V Value  `json:"v"`
}

// KV is one column assignment, used for both INSERT values and UPDATE SET.
type KV struct {
	C string `json:"c"`
	V Value  `json:"v"`
}

// Request is what db_nats publishes.
type Request struct {
	V      int      `json:"v"`
	Op     string   `json:"op"`
	Table  string   `json:"table,omitempty"`
	Cols   []string `json:"cols,omitempty"`
	Filter []Term   `json:"filter,omitempty"`
	Set    []KV     `json:"set,omitempty"`
	Values []KV     `json:"values,omitempty"`
	Order  string   `json:"order,omitempty"`
	Un     int      `json:"un,omitempty"`
	Fetch  bool     `json:"fetch,omitempty"`
	Limit  int      `json:"limit,omitempty"`
	Cursor string   `json:"cursor,omitempty"`
	SQL    string   `json:"sql,omitempty"`
}

// Col describes one result column.
type Col struct {
	N string `json:"n"`
	T string `json:"t"`
}

// Reply is what the backend sends back.
//
// Rows is row-major and positional against Cols. A nil element marshals to
// JSON null, which is how the C side recognises a SQL NULL.
type Reply struct {
	V        int         `json:"v"`
	OK       bool        `json:"ok"`
	Code     string      `json:"code,omitempty"`
	Err      string      `json:"err,omitempty"`
	Affected int         `json:"affected"`
	Cols     []Col       `json:"cols,omitempty"`
	Rows     [][]*string `json:"rows"`
	Cursor   string      `json:"cursor,omitempty"`
	// CursorSubject names the instance that owns Cursor. Continuation must be
	// addressed there: cursor state is process-local, so the load-balanced
	// queue subject would reach the wrong backend most of the time.
	CursorSubject string `json:"cursor_subject,omitempty"`
	Done          bool   `json:"done,omitempty"`
}

// Error codes surfaced to the C side.
const (
	CodeDenied        = "denied"
	CodeNoSuchTable   = "no_such_table"
	CodeBadRequest    = "bad_request"
	CodeBackendError  = "backend_error"
	CodeNoCursor      = "no_cursor"
	CodeReplyTooLarge = "reply_too_large"
)

func errorReply(code, msg string) Reply {
	return Reply{V: ProtoVersion, OK: false, Code: code, Err: msg}
}

func base64Encode(b []byte) string {
	return base64.StdEncoding.EncodeToString(b)
}

func base64Decode(s string) ([]byte, error) {
	return base64.StdEncoding.DecodeString(s)
}

package main

import (
	"strings"
	"unicode"
)

// Raw executes a statement that db_nats forwarded verbatim.
//
// The SQL is built by the Kamailio config (sqlops interpolates pseudo-
// variables with pv_printf_s), so this path cannot be parameterised here and
// escaping remains the config author's job, exactly as with db_mysql. Keep it
// disabled unless a module genuinely needs it, and prefer denying the .raw
// subject in the NATS account so the bus enforces it.
func (h *Handler) Raw(req *Request) Reply {
	if !h.cfg.NATS.EnableRaw {
		return errorReply(CodeDenied,
			"raw sql is disabled (set nats.enable_raw to allow it)")
	}
	if strings.TrimSpace(req.SQL) == "" {
		return errorReply(CodeBadRequest, "empty raw statement")
	}

	// db_nats already refuses these, but a different publisher on this
	// subject must not be able to strand a transaction on a pooled connection
	if isSessionSQL(req.SQL) {
		return errorReply(CodeDenied,
			"session-state sql cannot be served over stateless request/reply")
	}

	if !isReadStatement(req.SQL) {
		res, err := h.db.Exec(req.SQL)
		if err != nil {
			return errorReply(CodeBackendError, err.Error())
		}
		n, err := res.RowsAffected()
		if err != nil {
			n = 0
		}
		return Reply{V: ProtoVersion, OK: true, Affected: int(n), Rows: [][]*string{}}
	}

	rows, err := h.db.Query(req.SQL)
	if err != nil {
		return errorReply(CodeBackendError, err.Error())
	}
	defer rows.Close()

	cols, err := describe(rows, h.cfg.MySQL.UnsignedType)
	if err != nil {
		return errorReply(CodeBackendError, err.Error())
	}

	out, _, err := scanPage(rows, cols, h.cfg.Cursors.MaxRows, h.cfg.loc)
	if err != nil {
		return errorReply(CodeBackendError, err.Error())
	}
	return Reply{V: ProtoVersion, OK: true, Cols: cols, Rows: out}
}

// stripLeading removes whitespace and both SQL comment styles.
func stripLeading(s string) string {
	for {
		s = strings.TrimLeftFunc(s, unicode.IsSpace)
		switch {
		case strings.HasPrefix(s, "--"):
			if i := strings.IndexByte(s, '\n'); i >= 0 {
				s = s[i+1:]
				continue
			}
			return ""
		case strings.HasPrefix(s, "/*"):
			if i := strings.Index(s, "*/"); i >= 0 {
				s = s[i+2:]
				continue
			}
			return ""
		default:
			return s
		}
	}
}

func firstWord(s string) string {
	s = stripLeading(s)
	i := strings.IndexFunc(s, func(r rune) bool {
		return !(unicode.IsLetter(r) || r == '_')
	})
	if i < 0 {
		return strings.ToUpper(s)
	}
	return strings.ToUpper(s[:i])
}

func isReadStatement(s string) bool {
	switch firstWord(s) {
	case "SELECT", "SHOW", "DESC", "DESCRIBE", "EXPLAIN", "WITH":
		return true
	}
	return false
}

// isSessionSQL mirrors nats_is_session_sql() in nats_dbase.c. Both sides
// enforce it: the C side to avoid a pointless round trip, this side because
// the subject may have other publishers.
func isSessionSQL(s string) bool {
	body := stripLeading(s)
	up := strings.ToUpper(body)

	switch firstWord(body) {
	case "BEGIN", "START", "COMMIT", "ROLLBACK", "SAVEPOINT", "RELEASE",
		"LOCK", "UNLOCK":
		return true
	case "SET":
		if strings.Contains(up, "AUTOCOMMIT") ||
			strings.Contains(up, "SESSION") ||
			strings.Contains(up, "TRANSACTION") {
			return true
		}
	}

	// advisory locks and row locks are bound to one connection
	if strings.Contains(up, "GET_LOCK(") ||
		strings.Contains(up, "RELEASE_LOCK(") ||
		strings.Contains(up, "FOR UPDATE") {
		return true
	}
	return false
}

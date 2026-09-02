package main

import (
	"database/sql"
	"fmt"
	"strings"
	"time"
)

type Handler struct {
	db      *sql.DB
	grants  Grants
	cursors *CursorStore
	cfg     *Config
	// cursorSubject is this instance's private cursor subject, handed to
	// clients so they can address cursor continuation back to us.
	cursorSubject string
}

func NewHandler(db *sql.DB, cfg *Config, cursors *CursorStore, cursorSubject string) *Handler {
	return &Handler{
		db: db, grants: NewGrants(cfg.Grants), cursors: cursors,
		cfg: cfg, cursorSubject: cursorSubject,
	}
}

// Handle dispatches one structured request.
func (h *Handler) Handle(req *Request) Reply {
	if req.V != ProtoVersion {
		return errorReply(CodeBadRequest,
			fmt.Sprintf("unsupported protocol version %d", req.V))
	}

	switch req.Op {
	case "select":
		return h.doSelect(req)
	case "insert", "replace", "insert_update":
		return h.doInsert(req)
	case "update":
		return h.doUpdate(req)
	case "delete":
		return h.doDelete(req)
	default:
		return errorReply(CodeBadRequest, fmt.Sprintf("unknown op %q", req.Op))
	}
}

// buildWhere renders the WHERE clause with placeholders only.
func (h *Handler) buildWhere(req *Request) (string, []any, error) {
	if len(req.Filter) == 0 {
		return "", nil, nil
	}

	var sb strings.Builder
	args := make([]any, 0, len(req.Filter))
	sb.WriteString(" WHERE ")

	for i, t := range req.Filter {
		if i > 0 {
			sb.WriteString(" AND ")
		}
		op := sqlOperators[t.O]

		// NULL never compares equal with =, so translate it
		if t.V.N && (t.O == "=" || t.O == "<>" || t.O == "!=") {
			sb.WriteString(quoteIdent(t.C))
			if t.O == "=" {
				sb.WriteString(" IS NULL")
			} else {
				sb.WriteString(" IS NOT NULL")
			}
			continue
		}

		arg, err := decodeArg(t.V, h.cfg.loc)
		if err != nil {
			return "", nil, err
		}

		if t.O == "&" {
			// bitwise test, as used by usrloc for cflags
			sb.WriteString(fmt.Sprintf("(%s & ?) = ?", quoteIdent(t.C)))
			args = append(args, arg, arg)
			continue
		}

		sb.WriteString(quoteIdent(t.C))
		sb.WriteString(" ")
		sb.WriteString(op)
		sb.WriteString(" ?")
		args = append(args, arg)
	}

	return sb.String(), args, nil
}

func (h *Handler) doSelect(req *Request) Reply {
	g, err := h.grants.authorize(req)
	if err != nil {
		return errorReply(CodeDenied, err.Error())
	}

	proj := "*"
	if len(req.Cols) > 0 {
		parts := make([]string, len(req.Cols))
		for i, c := range req.Cols {
			parts[i] = quoteIdent(c)
		}
		proj = strings.Join(parts, ", ")
	}

	where, args, err := h.buildWhere(req)
	if err != nil {
		return errorReply(CodeBadRequest, err.Error())
	}

	q := "SELECT " + proj + " FROM " + quoteIdent(req.Table) + where

	if req.Order != "" {
		col, dir, err := parseOrder(req.Order)
		if err != nil {
			return errorReply(CodeBadRequest, err.Error())
		}
		q += " ORDER BY " + quoteIdent(col)
		if dir != "" {
			q += " " + dir
		}
	}

	// a grant may cap how much a single statement can return
	max := g.MaxRows
	if max <= 0 || max > h.cfg.Cursors.MaxRows {
		max = h.cfg.Cursors.MaxRows
	}

	rows, err := h.db.Query(q, args...)
	if err != nil {
		return errorReply(CodeBackendError, err.Error())
	}

	cols, err := describe(rows, h.cfg.MySQL.UnsignedType)
	if err != nil {
		rows.Close()
		return errorReply(CodeBackendError, err.Error())
	}

	// fetch mode: hand back only the metadata plus a cursor to page through
	if req.Fetch {
		c, err := h.cursors.Add(rows, cols)
		if err != nil {
			rows.Close()
			return errorReply(CodeBackendError, err.Error())
		}
		return Reply{
			V: ProtoVersion, OK: true, Cols: cols,
			Rows: [][]*string{}, Cursor: c.ID,
			CursorSubject: h.cursorSubject,
		}
	}

	defer rows.Close()
	out, _, err := scanPage(rows, cols, max, h.cfg.loc)
	if err != nil {
		return errorReply(CodeBackendError, err.Error())
	}
	return Reply{V: ProtoVersion, OK: true, Cols: cols, Rows: out}
}

// Fetch returns the next page of an open cursor.
func (h *Handler) Fetch(req *Request) Reply {
	c, ok := h.cursors.Get(req.Cursor)
	if !ok {
		return errorReply(CodeNoCursor, "unknown or expired cursor")
	}

	n := req.Limit
	if n <= 0 || n > h.cfg.Cursors.MaxRows {
		n = h.cfg.Cursors.MaxRows
	}

	out, exhausted, err := scanPage(c.rows, c.cols, n, h.cfg.loc)
	if err != nil {
		h.cursors.Close(req.Cursor)
		return errorReply(CodeBackendError, err.Error())
	}

	rep := Reply{V: ProtoVersion, OK: true, Cols: c.cols, Rows: out}
	if exhausted {
		h.cursors.Close(req.Cursor)
		rep.Done = true
	} else {
		rep.Cursor = c.ID
		rep.CursorSubject = h.cursorSubject
	}
	return rep
}

func (h *Handler) CloseCursor(req *Request) Reply {
	h.cursors.Close(req.Cursor)
	return Reply{V: ProtoVersion, OK: true, Rows: [][]*string{}, Done: true}
}

func (h *Handler) doInsert(req *Request) Reply {
	if _, err := h.grants.authorize(req); err != nil {
		return errorReply(CodeDenied, err.Error())
	}
	if len(req.Values) == 0 {
		return errorReply(CodeBadRequest, "insert without values")
	}

	cols := make([]string, len(req.Values))
	marks := make([]string, len(req.Values))
	args := make([]any, len(req.Values))
	for i, kv := range req.Values {
		cols[i] = quoteIdent(kv.C)
		marks[i] = "?"
		a, err := decodeArg(kv.V, h.cfg.loc)
		if err != nil {
			return errorReply(CodeBadRequest, err.Error())
		}
		args[i] = a
	}

	verb := "INSERT INTO"
	if req.Op == "replace" {
		verb = "REPLACE INTO"
	}

	q := verb + " " + quoteIdent(req.Table) +
		" (" + strings.Join(cols, ", ") + ") VALUES (" + strings.Join(marks, ", ") + ")"

	if req.Op == "insert_update" {
		ups := make([]string, len(cols))
		for i, c := range cols {
			ups[i] = c + " = VALUES(" + c + ")"
		}
		q += " ON DUPLICATE KEY UPDATE " + strings.Join(ups, ", ")
	}

	return h.exec(q, args)
}

func (h *Handler) doUpdate(req *Request) Reply {
	if _, err := h.grants.authorize(req); err != nil {
		return errorReply(CodeDenied, err.Error())
	}
	if len(req.Set) == 0 {
		return errorReply(CodeBadRequest, "update without assignments")
	}

	sets := make([]string, len(req.Set))
	args := make([]any, 0, len(req.Set)+len(req.Filter))
	for i, kv := range req.Set {
		sets[i] = quoteIdent(kv.C) + " = ?"
		a, err := decodeArg(kv.V, h.cfg.loc)
		if err != nil {
			return errorReply(CodeBadRequest, err.Error())
		}
		args = append(args, a)
	}

	where, wargs, err := h.buildWhere(req)
	if err != nil {
		return errorReply(CodeBadRequest, err.Error())
	}
	args = append(args, wargs...)

	q := "UPDATE " + quoteIdent(req.Table) + " SET " + strings.Join(sets, ", ") + where
	return h.exec(q, args)
}

func (h *Handler) doDelete(req *Request) Reply {
	if _, err := h.grants.authorize(req); err != nil {
		return errorReply(CodeDenied, err.Error())
	}

	where, args, err := h.buildWhere(req)
	if err != nil {
		return errorReply(CodeBadRequest, err.Error())
	}

	q := "DELETE FROM " + quoteIdent(req.Table) + where
	return h.exec(q, args)
}

func (h *Handler) exec(q string, args []any) Reply {
	res, err := h.db.Exec(q, args...)
	if err != nil {
		return errorReply(CodeBackendError, err.Error())
	}
	n, err := res.RowsAffected()
	if err != nil {
		n = 0
	}
	return Reply{V: ProtoVersion, OK: true, Affected: int(n), Rows: [][]*string{}}
}

// describe converts driver column metadata into the wire form. The declared
// types matter: db_check_table_version rejects a non-integer version column,
// and usrloc needs NUL-terminated strings, which only DB1_STRING provides.
func describe(rows *sql.Rows, unsignedType bool) ([]Col, error) {
	cts, err := rows.ColumnTypes()
	if err != nil {
		return nil, err
	}
	cols := make([]Col, len(cts))
	for i, ct := range cts {
		cols[i] = Col{N: ct.Name(), T: wireTypeFor(ct.DatabaseTypeName(), unsignedType)}
	}
	return cols, nil
}

// scanPage reads up to max rows. The bool reports whether the result set was
// exhausted, which is what ends the client's fetch loop.
func scanPage(rows *sql.Rows, cols []Col, max int, loc *time.Location) ([][]*string, bool, error) {
	out := make([][]*string, 0, 16)

	for len(out) < max {
		if !rows.Next() {
			return out, true, rows.Err()
		}

		raw := make([][]byte, len(cols))
		dest := make([]any, len(cols))
		for i := range raw {
			dest[i] = &raw[i]
		}
		if err := rows.Scan(dest...); err != nil {
			return nil, false, err
		}

		row := make([]*string, len(cols))
		for i, b := range raw {
			if b == nil {
				row[i] = nil // marshals to JSON null == SQL NULL
				continue
			}
			s, err := encodeCell(b, cols[i].T, loc)
			if err != nil {
				return nil, false, err
			}
			row[i] = &s
		}
		out = append(out, row)
	}

	return out, false, nil
}

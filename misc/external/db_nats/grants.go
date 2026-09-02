package main

import (
	"fmt"
	"strings"
)

// TableGrant is the authorization unit: what may be done to one table.
//
// Nothing is permitted by default. A request naming a table with no grant, an
// operation not listed, or a column outside the allowlist is refused before
// any SQL is built.
type TableGrant struct {
	Table   string   `yaml:"table"`
	Ops     []string `yaml:"ops"`
	Columns []string `yaml:"columns"`
	// RequireFilter names columns that must appear in the WHERE clause. Use it
	// to stop a full-table read or an unscoped DELETE.
	RequireFilter []string `yaml:"require_filter"`
	MaxRows       int      `yaml:"max_rows"`

	opsSet  map[string]bool
	colsSet map[string]bool
	allCols bool
}

var knownOps = map[string]bool{
	"select": true, "insert": true, "update": true,
	"delete": true, "replace": true, "insert_update": true,
}

func (g *TableGrant) normalize() error {
	if !identOK(g.Table) {
		return fmt.Errorf("grant has invalid table name %q", g.Table)
	}

	g.opsSet = make(map[string]bool, len(g.Ops))
	for _, op := range g.Ops {
		op = strings.ToLower(strings.TrimSpace(op))
		if !knownOps[op] {
			return fmt.Errorf("table %s: unknown op %q", g.Table, op)
		}
		g.opsSet[op] = true
	}

	g.colsSet = make(map[string]bool, len(g.Columns))
	for _, c := range g.Columns {
		c = strings.TrimSpace(c)
		if c == "*" {
			g.allCols = true
			continue
		}
		if !identOK(c) {
			return fmt.Errorf("table %s: invalid column name %q", g.Table, c)
		}
		g.colsSet[c] = true
	}
	if !g.allCols && len(g.colsSet) == 0 {
		return fmt.Errorf("table %s: no columns granted", g.Table)
	}

	for _, c := range g.RequireFilter {
		if !identOK(c) {
			return fmt.Errorf("table %s: invalid require_filter column %q", g.Table, c)
		}
	}
	return nil
}

func (g *TableGrant) allowsOp(op string) bool { return g.opsSet[op] }
func (g *TableGrant) allowsCol(c string) bool { return g.allCols || g.colsSet[c] }

// Grants indexes the configured table grants by name.
type Grants map[string]*TableGrant

func NewGrants(list []TableGrant) Grants {
	m := make(Grants, len(list))
	for i := range list {
		m[list[i].Table] = &list[i]
	}
	return m
}

// authorize is the single gate every structured request passes through.
func (gr Grants) authorize(req *Request) (*TableGrant, error) {
	if !identOK(req.Table) {
		return nil, fmt.Errorf("invalid table name")
	}
	g, ok := gr[req.Table]
	if !ok {
		return nil, fmt.Errorf("no grant for table %q", req.Table)
	}
	if !g.allowsOp(req.Op) {
		return nil, fmt.Errorf("op %q not granted on table %q", req.Op, req.Table)
	}

	// projection
	for _, c := range req.Cols {
		if !identOK(c) || !g.allowsCol(c) {
			return nil, fmt.Errorf("column %q not granted on table %q", c, req.Table)
		}
	}
	// WHERE terms
	for _, t := range req.Filter {
		if !identOK(t.C) || !g.allowsCol(t.C) {
			return nil, fmt.Errorf("filter column %q not granted on table %q", t.C, req.Table)
		}
		if _, ok := sqlOperators[t.O]; !ok {
			return nil, fmt.Errorf("operator %q not permitted", t.O)
		}
	}
	// written columns
	for _, kv := range append(append([]KV{}, req.Set...), req.Values...) {
		if !identOK(kv.C) || !g.allowsCol(kv.C) {
			return nil, fmt.Errorf("column %q not granted on table %q", kv.C, req.Table)
		}
	}
	// ORDER BY
	if req.Order != "" {
		col, _, err := parseOrder(req.Order)
		if err != nil {
			return nil, err
		}
		if !g.allowsCol(col) {
			return nil, fmt.Errorf("order column %q not granted on table %q", col, req.Table)
		}
	}

	if len(g.RequireFilter) > 0 {
		present := make(map[string]bool, len(req.Filter))
		for _, t := range req.Filter {
			present[t.C] = true
		}
		for _, need := range g.RequireFilter {
			if !present[need] {
				return nil, fmt.Errorf(
					"table %q requires a filter on column %q", req.Table, need)
			}
		}
	}

	return g, nil
}

// identOK accepts only bare SQL identifiers. Everything reaching the query
// builder as an identifier is checked here, so backtick quoting downstream
// cannot be escaped.
func identOK(s string) bool {
	if s == "" || len(s) > 64 {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'a' && c <= 'z':
		case c >= 'A' && c <= 'Z':
		case c >= '0' && c <= '9':
		case c == '_':
		default:
			return false
		}
	}
	return true
}

func quoteIdent(s string) string { return "`" + s + "`" }

// sqlOperators is the closed set of comparisons db_nats may request. It
// matches the OP_* macros in src/lib/srdb1/db_op.h.
var sqlOperators = map[string]string{
	"=":  "=",
	"<":  "<",
	">":  ">",
	"<=": "<=",
	">=": ">=",
	"<>": "<>",
	"!=": "<>",
	"&":  "&",
}

// parseOrder splits "col" or "col DESC" and validates both halves.
func parseOrder(o string) (col string, dir string, err error) {
	f := strings.Fields(strings.TrimSpace(o))
	switch len(f) {
	case 1:
		col = f[0]
	case 2:
		col = f[0]
		switch strings.ToUpper(f[1]) {
		case "ASC":
			dir = "ASC"
		case "DESC":
			dir = "DESC"
		default:
			return "", "", fmt.Errorf("invalid order direction %q", f[1])
		}
	default:
		return "", "", fmt.Errorf("invalid order clause %q", o)
	}
	if !identOK(col) {
		return "", "", fmt.Errorf("invalid order column %q", col)
	}
	return col, dir, nil
}

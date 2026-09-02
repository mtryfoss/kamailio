package main

import "testing"

func testGrants(t *testing.T) Grants {
	t.Helper()
	list := []TableGrant{
		{Table: "subscriber", Ops: []string{"select"},
			Columns: []string{"username", "domain", "ha1", "ha1b", "rpid"}},
		{Table: "location", Ops: []string{"select", "insert", "update", "delete"},
			Columns: []string{"*"}},
		{Table: "version", Ops: []string{"select"},
			Columns: []string{"table_name", "table_version"}},
		{Table: "scoped", Ops: []string{"select", "delete"},
			Columns: []string{"*"}, RequireFilter: []string{"username"}},
	}
	for i := range list {
		if err := list[i].normalize(); err != nil {
			t.Fatalf("normalize %s: %v", list[i].Table, err)
		}
	}
	return NewGrants(list)
}

func TestAuthorizeAllowsGrantedRequest(t *testing.T) {
	gr := testGrants(t)
	req := &Request{V: 1, Op: "select", Table: "subscriber",
		Cols:   []string{"ha1"},
		Filter: []Term{{C: "username", O: "=", V: Value{T: TypeString, D: "a"}}}}
	if _, err := gr.authorize(req); err != nil {
		t.Fatalf("expected allow, got %v", err)
	}
}

func TestAuthorizeDenials(t *testing.T) {
	gr := testGrants(t)
	cases := []struct {
		name string
		req  *Request
	}{
		{"ungranted table", &Request{Op: "select", Table: "acc"}},
		{"ungranted op", &Request{Op: "delete", Table: "subscriber"}},
		{"ungranted column in projection",
			&Request{Op: "select", Table: "subscriber", Cols: []string{"password"}}},
		{"ungranted column in filter",
			&Request{Op: "select", Table: "subscriber",
				Filter: []Term{{C: "secret", O: "=", V: Value{T: TypeString}}}}},
		{"ungranted column in set",
			&Request{Op: "update", Table: "subscriber",
				Set: []KV{{C: "ha1", V: Value{T: TypeString}}}}},
		{"bad operator",
			&Request{Op: "select", Table: "subscriber",
				Filter: []Term{{C: "username", O: "LIKE", V: Value{T: TypeString}}}}},
		{"sql injection via table", &Request{Op: "select", Table: "sub`; DROP TABLE x--"}},
		{"sql injection via column",
			&Request{Op: "select", Table: "location", Cols: []string{"a`,(SELECT 1)"}}},
		{"injection via order",
			&Request{Op: "select", Table: "location", Order: "a; DROP TABLE x"}},
		{"missing required filter",
			&Request{Op: "delete", Table: "scoped"}},
	}
	for _, c := range cases {
		if _, err := gr.authorize(c.req); err == nil {
			t.Errorf("%s: expected denial, got allow", c.name)
		}
	}
}

// A wildcard grant still must not let an invalid identifier through, or
// backtick quoting downstream could be escaped.
func TestWildcardStillValidatesIdentifiers(t *testing.T) {
	gr := testGrants(t)
	req := &Request{Op: "select", Table: "location", Cols: []string{"ok_col", "bad`col"}}
	if _, err := gr.authorize(req); err == nil {
		t.Fatal("wildcard grant must still reject a malformed identifier")
	}
}

func TestIdentOK(t *testing.T) {
	good := []string{"username", "table_name", "a1", "_x", "A_B_9"}
	bad := []string{"", "a b", "a`b", "a'b", "a;b", "a-b", "a.b", "a*", "a\x00b",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
	for _, s := range good {
		if !identOK(s) {
			t.Errorf("identOK(%q) = false, want true", s)
		}
	}
	for _, s := range bad {
		if identOK(s) {
			t.Errorf("identOK(%q) = true, want false", s)
		}
	}
}

func TestParseOrder(t *testing.T) {
	if c, d, err := parseOrder("q"); err != nil || c != "q" || d != "" {
		t.Fatalf("plain column: %q %q %v", c, d, err)
	}
	if c, d, err := parseOrder("last_modified DESC"); err != nil || c != "last_modified" || d != "DESC" {
		t.Fatalf("desc: %q %q %v", c, d, err)
	}
	for _, bad := range []string{"a; DROP TABLE x", "a SIDEWAYS", "a b c", "`a`"} {
		if _, _, err := parseOrder(bad); err == nil {
			t.Errorf("parseOrder(%q) should have failed", bad)
		}
	}
}

func TestGrantWithNoColumnsIsRejected(t *testing.T) {
	g := TableGrant{Table: "t", Ops: []string{"select"}}
	if err := g.normalize(); err == nil {
		t.Fatal("a grant with no columns must be rejected, not treated as allow-all")
	}
}

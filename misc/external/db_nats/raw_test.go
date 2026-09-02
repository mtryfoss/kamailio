package main

import "testing"

// These must agree with nats_is_session_sql() in nats_dbase.c. Both sides
// enforce the rule, so a divergence would let something through.
func TestIsSessionSQL(t *testing.T) {
	session := []string{
		"BEGIN",
		"START TRANSACTION",
		"COMMIT",
		"ROLLBACK",
		"SAVEPOINT sp1",
		"RELEASE SAVEPOINT sp1",
		"SET AUTOCOMMIT=0",
		"SET autocommit = 1",
		"SET SESSION sql_mode='ANSI'",
		"SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
		"LOCK TABLES t WRITE",
		"UNLOCK TABLES",
		"SELECT GET_LOCK('x',1)",
		"SELECT RELEASE_LOCK('x')",
		"SELECT * FROM t FOR UPDATE",
		"  \n\t COMMIT",
		"-- a comment\nCOMMIT",
		"/* block */ ROLLBACK",
	}
	for _, s := range session {
		if !isSessionSQL(s) {
			t.Errorf("isSessionSQL(%q) = false, want true", s)
		}
	}

	ordinary := []string{
		"SELECT * FROM subscriber WHERE username='a'",
		"INSERT INTO location (ruid) VALUES ('x')",
		"UPDATE location SET expires=1 WHERE id=2",
		"DELETE FROM location WHERE expires<1",
		// word-boundary check: these must not trip the COMMIT/SET keywords
		"SELECT committed FROM t",
		"SELECT * FROM settings",
		"SELECT beginning FROM t",
	}
	for _, s := range ordinary {
		if isSessionSQL(s) {
			t.Errorf("isSessionSQL(%q) = true, want false", s)
		}
	}
}

func TestIsReadStatement(t *testing.T) {
	reads := []string{"SELECT 1", "  select 1", "SHOW TABLES", "EXPLAIN SELECT 1",
		"WITH x AS (SELECT 1) SELECT * FROM x", "/* c */ SELECT 1"}
	for _, s := range reads {
		if !isReadStatement(s) {
			t.Errorf("isReadStatement(%q) = false, want true", s)
		}
	}
	writes := []string{"INSERT INTO t VALUES (1)", "UPDATE t SET a=1", "DELETE FROM t"}
	for _, s := range writes {
		if isReadStatement(s) {
			t.Errorf("isReadStatement(%q) = true, want false", s)
		}
	}
}

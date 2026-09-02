package main

import (
	"strconv"
	"testing"
	"time"
)

func TestWireTypeFor(t *testing.T) {
	cases := map[string]string{
		"INT": TypeInt, "INTEGER": TypeInt, "TINYINT": TypeInt,
		"SMALLINT": TypeInt, "MEDIUMINT": TypeInt, "YEAR": TypeInt,
		"BIGINT": TypeBigint, "FLOAT": TypeDouble,
		"DOUBLE": TypeDouble, "DATETIME": TypeDatetime,
		"DATE": TypeDatetime, "BLOB": TypeBlob, "LONGBLOB": TypeBlob,
		"VARBINARY": TypeBlob, "SET": TypeBitmap,
		// text and anything unknown must be DB1_STRING, never DB1_STR:
		// db_str2val only NUL-terminates the DB1_STRING copy, and usrloc
		// reads values with VAL_STRING()+strlen()
		"VARCHAR": TypeString, "CHAR": TypeString, "TEXT": TypeString,
		"DECIMAL": TypeString, "JSON": TypeString, "WEIRDTYPE": TypeString,
		// deliberate divergence from db_mysql, which maps this to DB1_INT
		// and then cannot parse the datetime string it receives
		"TIMESTAMP": TypeDatetime,
	}
	for in, want := range cases {
		if got := wireTypeFor(in, false); got != want {
			t.Errorf("wireTypeFor(%q) = %q, want %q", in, got, want)
		}
	}
}

// db_mysql reports UNSIGNED columns as DB1_INT unless its unsigned_type
// modparam is set. The backend defaults the same way.
func TestWireTypeForUnsigned(t *testing.T) {
	if got := wireTypeFor("UNSIGNED INT", false); got != TypeInt {
		t.Errorf("unsigned INT with flag off = %q, want %q", got, TypeInt)
	}
	if got := wireTypeFor("UNSIGNED BIGINT", false); got != TypeBigint {
		t.Errorf("unsigned BIGINT with flag off = %q, want %q", got, TypeBigint)
	}
	if got := wireTypeFor("UNSIGNED INT", true); got != TypeUint {
		t.Errorf("unsigned INT with flag on = %q, want %q", got, TypeUint)
	}
	if got := wireTypeFor("UNSIGNED BIGINT", true); got != TypeUbigint {
		t.Errorf("unsigned BIGINT with flag on = %q, want %q", got, TypeUbigint)
	}
}

func TestEncodeCellDatetime(t *testing.T) {
	loc := time.UTC
	got, err := encodeCell([]byte("2026-09-01 12:34:56"), TypeDatetime, loc)
	if err != nil {
		t.Fatalf("encodeCell: %v", err)
	}
	want := strconv.FormatInt(time.Date(2026, 9, 1, 12, 34, 56, 0, loc).Unix(), 10)
	if got != want {
		t.Fatalf("got %q want %q", got, want)
	}

	// MySQL's unset-datetime sentinel must not blow up
	if got, err := encodeCell([]byte("0000-00-00 00:00:00"), TypeDatetime, loc); err != nil || got != "0" {
		t.Fatalf("zero datetime: got %q err %v", got, err)
	}
	// fractional seconds are truncated to DB1_DATETIME's resolution
	if _, err := encodeCell([]byte("2026-09-01 12:34:56.789"), TypeDatetime, loc); err != nil {
		t.Fatalf("fractional seconds: %v", err)
	}
}

func TestDatetimeRoundTrip(t *testing.T) {
	loc := time.UTC
	const epoch = "1772541296"
	arg, err := decodeArg(Value{T: TypeDatetime, D: epoch}, loc)
	if err != nil {
		t.Fatalf("decodeArg: %v", err)
	}
	back, err := encodeCell([]byte(arg.(string)), TypeDatetime, loc)
	if err != nil {
		t.Fatalf("encodeCell: %v", err)
	}
	if back != epoch {
		t.Fatalf("round trip changed value: %q -> %q", epoch, back)
	}
}

func TestBlobRoundTrip(t *testing.T) {
	orig := []byte{0x00, 0x01, 0xff, 0xfe, 'a', 0x00}
	enc, err := encodeCell(orig, TypeBlob, time.UTC)
	if err != nil {
		t.Fatalf("encodeCell: %v", err)
	}
	arg, err := decodeArg(Value{T: TypeBlob, D: enc}, time.UTC)
	if err != nil {
		t.Fatalf("decodeArg: %v", err)
	}
	if string(arg.([]byte)) != string(orig) {
		t.Fatalf("blob round trip failed: %v -> %v", orig, arg)
	}
}

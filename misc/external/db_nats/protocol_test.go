package main

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

// The JSON fixtures below are the exact shapes nats_proto.c emits. If the C
// encoder changes, these fail - that is the point.

func TestDecodesCEncoderSelect(t *testing.T) {
	raw := `{"v":1,"op":"select","table":"subscriber",` +
		`"cols":["ha1","rpid"],` +
		`"filter":[{"c":"username","o":"=","v":{"t":"string","d":"alice"}},` +
		`{"c":"domain","o":"=","v":{"t":"str","d":"example.org"}}],` +
		`"order":"q","fetch":true}`

	var req Request
	if err := json.Unmarshal([]byte(raw), &req); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if req.V != ProtoVersion || req.Op != "select" || req.Table != "subscriber" {
		t.Fatalf("envelope mismatch: %+v", req)
	}
	if len(req.Cols) != 2 || req.Cols[0] != "ha1" {
		t.Fatalf("cols mismatch: %v", req.Cols)
	}
	if len(req.Filter) != 2 || req.Filter[1].V.D != "example.org" {
		t.Fatalf("filter mismatch: %+v", req.Filter)
	}
	if !req.Fetch || req.Order != "q" {
		t.Fatalf("fetch/order mismatch: %+v", req)
	}
}

func TestDecodesCEncoderNullValue(t *testing.T) {
	raw := `{"v":1,"op":"update","table":"location",` +
		`"set":[{"c":"received","v":{"t":"string","n":true}}],` +
		`"filter":[{"c":"expires","o":"<","v":{"t":"datetime","d":"1712345678"}}]}`

	var req Request
	if err := json.Unmarshal([]byte(raw), &req); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if !req.Set[0].V.N {
		t.Fatal("expected null flag on set value")
	}
	arg, err := decodeArg(req.Set[0].V, time.UTC)
	if err != nil {
		t.Fatalf("decodeArg: %v", err)
	}
	if arg != nil {
		t.Fatalf("null value must decode to nil, got %#v", arg)
	}
}

// A SQL NULL must marshal as JSON null: the C decoder recognises exactly that
// (srjson_NULL) and builds a value with VAL_NULL set.
func TestReplyNullMarshalsAsJSONNull(t *testing.T) {
	s := "alice"
	rep := Reply{
		V: ProtoVersion, OK: true,
		Cols: []Col{{N: "username", T: TypeString}, {N: "rpid", T: TypeString}},
		Rows: [][]*string{{&s, nil}},
	}
	b, err := json.Marshal(rep)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	if !strings.Contains(string(b), `"rows":[["alice",null]]`) {
		t.Fatalf("expected a JSON null cell, got %s", string(b))
	}
}

// Everything travels as a string precisely so this survives: srjson stores
// numbers as double, so a JSON-number bigint would be silently corrupted.
func TestBigintPrecisionSurvives(t *testing.T) {
	const big = "9007199254740993" // 2^53 + 1
	arg, err := decodeArg(Value{T: TypeBigint, D: big}, time.UTC)
	if err != nil {
		t.Fatalf("decodeArg: %v", err)
	}
	if got, ok := arg.(int64); !ok || got != 9007199254740993 {
		t.Fatalf("bigint corrupted: %#v", arg)
	}
}

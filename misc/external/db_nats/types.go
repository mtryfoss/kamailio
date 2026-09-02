package main

import (
	"fmt"
	"strconv"
	"strings"
	"time"
)

// Wire type names. These mirror the DB1_* enum in
// src/lib/srdb1/db_val.h, which is what the C side decodes into.
const (
	TypeInt      = "int"
	TypeUint     = "uint"
	TypeBigint   = "bigint"
	TypeUbigint  = "ubigint"
	TypeDouble   = "double"
	TypeString   = "string"
	TypeStr      = "str"
	TypeDatetime = "datetime"
	TypeBlob     = "blob"
	TypeBitmap   = "bitmap"
)

// mysqlLayout is how MySQL renders DATETIME/TIMESTAMP with parseTime=false.
const mysqlLayout = "2006-01-02 15:04:05"

// wireTypeFor maps a MySQL column type to the DB1_* type the C driver should
// see. It deliberately mirrors db_mysql_get_columns() in
// src/modules/db_mysql/km_res.c so that swapping drivers does not change the
// db_val_t types a module observes.
//
// Two deliberate choices:
//
//   - Text and anything unrecognised become "string" (DB1_STRING), never "str"
//     (DB1_STR). db_str2val() only NUL-terminates the DB1_STRING copy, and
//     usrloc/auth_db read values with VAL_STRING()+strlen().
//
//   - TIMESTAMP becomes "datetime", where db_mysql maps it to DB1_INT. That
//     db_mysql mapping cannot actually work: MySQL hands back a formatted
//     datetime string and db_str2val() would try to parse it as an integer.
//     Treating it as a datetime is the behaviour callers expect.
//
// unsignedType mirrors db_mysql's modparam of the same name. db_mysql
// defaults it off, reporting UNSIGNED columns as DB1_INT/DB1_BIGINT, so this
// defaults off too. The knob lives here rather than in the C module because
// the backend is what inspects the column metadata.
func wireTypeFor(dbType string, unsignedType bool) string {
	t := strings.ToUpper(strings.TrimSpace(dbType))
	unsigned := strings.HasPrefix(t, "UNSIGNED ") && unsignedType
	t = strings.TrimPrefix(t, "UNSIGNED ")

	switch t {
	case "TINYINT", "SMALLINT", "MEDIUMINT", "INT", "INTEGER", "YEAR":
		if unsigned {
			return TypeUint
		}
		return TypeInt
	case "BIGINT":
		if unsigned {
			return TypeUbigint
		}
		return TypeBigint
	case "FLOAT", "DOUBLE":
		return TypeDouble
	case "DATETIME", "TIMESTAMP", "DATE":
		return TypeDatetime
	case "BLOB", "TINYBLOB", "MEDIUMBLOB", "LONGBLOB", "BINARY", "VARBINARY":
		return TypeBlob
	case "SET":
		return TypeBitmap
	default:
		// includes VARCHAR, CHAR, TEXT, DECIMAL, JSON, ENUM, ...
		return TypeString
	}
}

// encodeCell turns a raw MySQL cell into the string form the wire uses.
// Every datum travels as a string so that 64-bit values survive the C side's
// JSON parser, which stores all numbers as double.
func encodeCell(raw []byte, wireType string, loc *time.Location) (string, error) {
	switch wireType {
	case TypeDatetime:
		s := string(raw)
		if s == "" {
			return "0", nil
		}
		// MySQL uses this sentinel for an unset DATETIME
		if strings.HasPrefix(s, "0000-00-00") {
			return "0", nil
		}
		layout := mysqlLayout
		if len(s) == len("2006-01-02") {
			layout = "2006-01-02"
		} else if i := strings.IndexByte(s, '.'); i >= 0 {
			// drop fractional seconds; DB1_DATETIME has second resolution
			s = s[:i]
		}
		t, err := time.ParseInLocation(layout, s, loc)
		if err != nil {
			return "", fmt.Errorf("cannot parse datetime %q: %w", s, err)
		}
		return strconv.FormatInt(t.Unix(), 10), nil

	case TypeBlob:
		return base64Encode(raw), nil

	default:
		return string(raw), nil
	}
}

// decodeArg converts an inbound wire value into an argument for a placeholder.
func decodeArg(v Value, loc *time.Location) (any, error) {
	if v.N {
		return nil, nil
	}

	switch v.T {
	case TypeInt, TypeBigint:
		n, err := strconv.ParseInt(v.D, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("bad %s value %q: %w", v.T, v.D, err)
		}
		return n, nil

	case TypeUint, TypeUbigint, TypeBitmap:
		n, err := strconv.ParseUint(v.D, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("bad %s value %q: %w", v.T, v.D, err)
		}
		return n, nil

	case TypeDouble:
		f, err := strconv.ParseFloat(v.D, 64)
		if err != nil {
			return nil, fmt.Errorf("bad double value %q: %w", v.D, err)
		}
		return f, nil

	case TypeDatetime:
		sec, err := strconv.ParseInt(v.D, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("bad datetime value %q: %w", v.D, err)
		}
		// the C side speaks epoch seconds; render in the configured zone
		return time.Unix(sec, 0).In(loc).Format(mysqlLayout), nil

	case TypeBlob:
		b, err := base64Decode(v.D)
		if err != nil {
			return nil, fmt.Errorf("bad blob value: %w", err)
		}
		return b, nil

	case TypeString, TypeStr:
		return v.D, nil

	default:
		return nil, fmt.Errorf("unsupported value type %q", v.T)
	}
}

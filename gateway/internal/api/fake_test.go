package api

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"arca/gateway/internal/resp"
)

type fakeEntry struct {
	value  string
	expire time.Time
}

type fakeCol struct {
	name    string
	typ     string
	pk      bool
	notNull bool
	auto    bool
	def     any
}

type fakeIndex struct {
	name   string
	column string
}

type fakeTable struct {
	name    string
	cols    []fakeCol
	rows    []map[string]any
	autoinc int64
	indexes []fakeIndex
}

type fakeArca struct {
	t        *testing.T
	ln       net.Listener
	password string
	mu       sync.Mutex
	kv       map[string]fakeEntry
	tables   map[string]*fakeTable
	config   map[string]string
	commands int64
	calls    map[string]int
	down     bool

	checkpoints map[string]int64
}

func newFakeArca(t *testing.T, password string) *fakeArca {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeArca{
		t: t, ln: ln, password: password,
		kv:     map[string]fakeEntry{},
		tables: map[string]*fakeTable{},
		config: map[string]string{"appendfsync": "everysec", "maxclients": "10000", "requirepass": password, "slowlog-log-slower-than": "10000"},
		calls:  map[string]int{},

		checkpoints: map[string]int64{},
	}
	go f.serve()
	t.Cleanup(func() { ln.Close() })
	return f
}

func (f *fakeArca) addr() string { return f.ln.Addr().String() }

func (f *fakeArca) serve() {
	for {
		c, err := f.ln.Accept()
		if err != nil {
			return
		}
		go f.handle(c)
	}
}

func (f *fakeArca) handle(c net.Conn) {
	defer c.Close()
	r := bufio.NewReader(c)
	w := bufio.NewWriter(c)
	authed := f.password == ""
	for {
		args, err := resp.ReadCommand(r)
		if err != nil {
			return
		}
		var reply resp.Value
		name := strings.ToUpper(args[0])
		switch {
		case name == "AUTH":
			if len(args) == 2 && args[1] == f.password {
				authed = true
				reply = resp.Value{Kind: resp.SimpleString, Str: "OK"}
			} else {
				reply = resp.Value{Kind: resp.Error, Str: "WRONGPASS senha inválida"}
			}
		case !authed:
			reply = resp.Value{Kind: resp.Error, Str: "NOAUTH autenticação necessária"}
		default:
			reply = f.exec(args)
		}
		if err := resp.WriteValue(w, reply); err != nil {
			return
		}
		if err := w.Flush(); err != nil {
			return
		}
	}
}

func okv() resp.Value                 { return resp.Value{Kind: resp.SimpleString, Str: "OK"} }
func errv(msg string) resp.Value      { return resp.Value{Kind: resp.Error, Str: "ERR " + msg} }
func bulk(s string) resp.Value        { return resp.Value{Kind: resp.Bulk, Str: s} }
func intv(n int64) resp.Value         { return resp.Value{Kind: resp.Integer, Int: n} }
func nullv() resp.Value               { return resp.Value{Kind: resp.Null} }
func jsonv(v any) resp.Value          { b, _ := json.Marshal(v); return bulk(string(b)) }
func arrv(v ...resp.Value) resp.Value { return resp.Value{Kind: resp.Array, Array: v} }

func (f *fakeArca) live(key string) (fakeEntry, bool) {
	e, ok := f.kv[key]
	if !ok {
		return e, false
	}
	if !e.expire.IsZero() && time.Now().After(e.expire) {
		delete(f.kv, key)
		return e, false
	}
	return e, true
}

func (f *fakeArca) exec(args []string) resp.Value {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.commands++
	name := strings.ToUpper(args[0])
	f.calls[name]++
	switch name {
	case "PING":
		return resp.Value{Kind: resp.SimpleString, Str: "PONG"}
	case "GET":
		e, ok := f.live(args[1])
		if !ok {
			return nullv()
		}
		return bulk(e.value)
	case "SET":
		e := fakeEntry{value: args[2]}
		nx, xx := false, false
		for i := 3; i < len(args); i++ {
			switch strings.ToUpper(args[i]) {
			case "NX":
				nx = true
			case "XX":
				xx = true
			case "EX", "PX":
				if i+1 < len(args) {
					n, _ := strconv.ParseInt(args[i+1], 10, 64)
					unit := time.Second
					if strings.ToUpper(args[i]) == "PX" {
						unit = time.Millisecond
					}
					e.expire = time.Now().Add(time.Duration(n) * unit)
					i++
				}
			}
		}
		_, exists := f.live(args[1])
		if (nx && exists) || (xx && !exists) {
			return nullv()
		}
		f.kv[args[1]] = e
		return okv()
	case "MGET":
		out := make([]resp.Value, 0, len(args)-1)
		for _, k := range args[1:] {
			if e, ok := f.live(k); ok {
				out = append(out, bulk(e.value))
			} else {
				out = append(out, nullv())
			}
		}
		return arrv(out...)
	case "ARCA.CHECKPOINTS":
		if len(args) == 3 && strings.ToUpper(args[1]) == "DELETE" {
			if _, ok := f.checkpoints[args[2]]; !ok {
				return errv("checkpoint '" + args[2] + "' não existe")
			}
			delete(f.checkpoints, args[2])
			return okv()
		}
		names := make([]string, 0, len(f.checkpoints))
		for n := range f.checkpoints {
			names = append(names, n)
		}
		sort.Strings(names)
		out := []map[string]any{}
		for _, n := range names {
			out = append(out, map[string]any{"name": n, "created_ms": f.checkpoints[n], "bytes": 4096, "files": 3})
		}
		return jsonv(out)
	case "ARCA.VERIFY":
		return jsonv(map[string]any{"files": 9, "bytes": 3145728, "ok": 9, "corrupt": []any{}, "ms": 4.2})
	case "DEL":
		var n int64
		for _, k := range args[1:] {
			if _, ok := f.live(k); ok {
				delete(f.kv, k)
				n++
			}
		}
		return intv(n)
	case "EXISTS":
		var n int64
		for _, k := range args[1:] {
			if _, ok := f.live(k); ok {
				n++
			}
		}
		return intv(n)
	case "PTTL":
		e, ok := f.live(args[1])
		if !ok {
			return intv(-2)
		}
		if e.expire.IsZero() {
			return intv(-1)
		}
		return intv(time.Until(e.expire).Milliseconds())
	case "PEXPIRE":
		e, ok := f.live(args[1])
		if !ok {
			return intv(0)
		}
		n, _ := strconv.ParseInt(args[2], 10, 64)
		e.expire = time.Now().Add(time.Duration(n) * time.Millisecond)
		f.kv[args[1]] = e
		return intv(1)
	case "PERSIST":
		e, ok := f.live(args[1])
		if !ok || e.expire.IsZero() {
			return intv(0)
		}
		e.expire = time.Time{}
		f.kv[args[1]] = e
		return intv(1)
	case "ARCA.SCANJSON":
		return f.scan(args)
	case "ARCA.STATS":
		return jsonv(f.stats())
	case "ARCA.FLUSH":
		return okv()
	case "ARCA.COMPACT":
		return okv()
	case "ARCA.CHECKPOINT":
		f.checkpoints[args[len(args)-1]] = time.Now().UnixMilli()
		return bulk("/dados/checkpoints/" + args[len(args)-1])
	case "CLIENT":
		return bulk("id=1 addr=127.0.0.1:50000 name=painel age=10 idle=0 cmd=client\nid=2 addr=127.0.0.1:50001 name= age=3 idle=2 cmd=get\n")
	case "CONFIG":
		if strings.ToUpper(args[1]) == "GET" {
			var out []resp.Value
			names := make([]string, 0, len(f.config))
			for k := range f.config {
				names = append(names, k)
			}
			sort.Strings(names)
			for _, k := range names {
				out = append(out, bulk(k), bulk(f.config[k]))
			}
			return arrv(out...)
		}
		if _, ok := f.config[args[2]]; !ok {
			return errv("parâmetro desconhecido '" + args[2] + "'")
		}
		f.config[args[2]] = args[3]
		return okv()
	case "SLOWLOG":
		return arrv(arrv(intv(7), intv(1761170000), intv(15000), arrv(bulk("SQL.JSON"), bulk("SELECT * FROM pedidos")), bulk("127.0.0.1:50001"), bulk("")))
	case "SQL.JSON":
		return f.sql(args[1])
	case "SQL.TABLES":
		return jsonv(f.tableList())
	}
	return errv("comando desconhecido '" + args[0] + "'")
}

func globMatch(pattern, s string) bool {
	if pattern == "" {
		return s == ""
	}
	switch pattern[0] {
	case '*':
		for i := 0; i <= len(s); i++ {
			if globMatch(pattern[1:], s[i:]) {
				return true
			}
		}
		return false
	case '?':
		return s != "" && globMatch(pattern[1:], s[1:])
	}
	return s != "" && s[0] == pattern[0] && globMatch(pattern[1:], s[1:])
}

func (f *fakeArca) scan(args []string) resp.Value {
	start, _ := strconv.Atoi(args[1])
	count, _ := strconv.Atoi(args[3])
	keys := make([]string, 0, len(f.kv))
	for k := range f.kv {
		if _, ok := f.live(k); ok {
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)
	type item struct {
		Key   string `json:"key"`
		Type  string `json:"type"`
		TTLMs int64  `json:"ttl_ms"`
		Size  int    `json:"size"`
	}
	out := []item{}
	i := start
	for ; i < len(keys) && len(out) < count; i++ {
		if !globMatch(args[2], keys[i]) {
			continue
		}
		e := f.kv[keys[i]]
		ttl := int64(-1)
		if !e.expire.IsZero() {
			ttl = time.Until(e.expire).Milliseconds()
		}
		out = append(out, item{Key: keys[i], Type: "string", TTLMs: ttl, Size: len(e.value)})
	}
	cursor := "0"
	if i < len(keys) {
		cursor = strconv.Itoa(i)
	}
	return jsonv(map[string]any{"cursor": cursor, "keys": out})
}

func (f *fakeArca) stats() map[string]any {
	levels := []map[string]any{}
	number := int64(40)
	for l := 0; l < 7; l++ {
		files := []int{2, 3, 4, 0, 0, 0, 0}[l]
		tables := []any{}
		var total int64
		for i := 0; i < files; i++ {
			size := int64(256<<10) * int64(l+1) * int64(i+2)
			total += size
			number++
			tables = append(tables, map[string]any{"number": number, "bytes": size, "smallest": fmt.Sprintf("kv:cache:produto:%d", i*10), "largest": fmt.Sprintf("row:pedidos:%08d", (i+1)*100), "entries": size / 64, "deletions": size / 4096})
		}
		maxBytes := int64(0)
		if l > 0 {
			maxBytes = int64(10<<20) * int64(l*10)
		}
		levels = append(levels, map[string]any{"level": l, "files": files, "bytes": total, "max_bytes": maxBytes, "score": float64(files) / 4, "tables": tables})
	}
	return map[string]any{
		"server":             map[string]any{"version": "1.0.0", "port": 7379, "uptime_s": 10, "io_threads": 2, "worker_threads": 4, "appendfsync": "everysec", "dir": "/dados"},
		"clients":            map[string]any{"connected": 2, "total": 5, "rejected": 0},
		"commands":           map[string]any{"total": f.commands, "ops_per_sec": 12.5, "reads": f.commands, "writes": f.commands / 2, "keyspace_hits": 10, "keyspace_misses": 2, "expired_keys": 0, "net_input_bytes": 100, "net_output_bytes": 200},
		"engine":             map[string]any{"last_sequence": f.commands, "memtable_bytes": 4096, "memtable_entries": 10, "immutable_bytes": 0, "wal_number": 3, "wal_bytes": 1024, "sst_files": 3, "sst_bytes": 3145728, "flushes": 1, "compactions": 1, "compaction_bytes_read": 10, "compaction_bytes_written": 10, "user_bytes_written": 10, "write_amplification": 1.5, "write_stalls": 0, "bloom_checks": 10, "bloom_useful": 9, "block_cache_bytes": 100, "block_cache_capacity": 1000, "block_cache_hits": f.commands, "block_cache_misses": 1, "compacting": false, "snapshots": 0, "estimated_keys": len(f.kv)},
		"levels":             levels,
		"recent_compactions": []any{map[string]any{"from_level": 0, "to_level": 1, "inputs": 4, "outputs": 2, "bytes_read": 4194304, "bytes_written": 3145728, "ms": 18.4, "at": time.Now().Add(-90 * time.Second).UnixMilli(), "reason": "L0"}},
		"keyspace":           map[string]any{"tables": len(f.tables)},
	}
}

func (f *fakeArca) tableList() []map[string]any {
	names := make([]string, 0, len(f.tables))
	for n := range f.tables {
		names = append(names, n)
	}
	sort.Strings(names)
	out := []map[string]any{}
	for _, n := range names {
		t := f.tables[n]
		cols := []map[string]any{}
		for _, c := range t.cols {
			cols = append(cols, map[string]any{"name": c.name, "type": c.typ, "primary_key": c.pk, "not_null": c.notNull || c.pk, "default": c.def, "auto_increment": c.auto})
		}
		idx := []map[string]any{}
		for _, i := range t.indexes {
			idx = append(idx, map[string]any{"name": i.name, "column": i.column})
		}
		out = append(out, map[string]any{"name": t.name, "columns": cols, "indexes": idx, "rows": len(t.rows)})
	}
	return out
}

type tok struct {
	kind string
	text string
	val  any
}

func lex(q string) ([]tok, error) {
	var out []tok
	i := 0
	for i < len(q) {
		c := q[i]
		switch {
		case c == ' ' || c == '\t' || c == '\n' || c == '\r':
			i++
		case c == '\'':
			var sb strings.Builder
			i++
			for {
				if i >= len(q) {
					return nil, errors.New("texto sem aspas de fechamento")
				}
				if q[i] == '\'' {
					if i+1 < len(q) && q[i+1] == '\'' {
						sb.WriteByte('\'')
						i += 2
						continue
					}
					i++
					break
				}
				sb.WriteByte(q[i])
				i++
			}
			out = append(out, tok{kind: "str", text: sb.String(), val: sb.String()})
		case c >= '0' && c <= '9' || c == '-' && i+1 < len(q) && q[i+1] >= '0' && q[i+1] <= '9':
			j := i + 1
			for j < len(q) && (q[j] >= '0' && q[j] <= '9' || q[j] == '.') {
				j++
			}
			text := q[i:j]
			var v any
			if strings.Contains(text, ".") {
				v, _ = strconv.ParseFloat(text, 64)
			} else {
				v, _ = strconv.ParseInt(text, 10, 64)
			}
			out = append(out, tok{kind: "num", text: text, val: v})
			i = j
		case c == '_' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z':
			j := i + 1
			for j < len(q) && (q[j] == '_' || q[j] >= 'a' && q[j] <= 'z' || q[j] >= 'A' && q[j] <= 'Z' || q[j] >= '0' && q[j] <= '9') {
				j++
			}
			out = append(out, tok{kind: "ident", text: q[i:j]})
			i = j
		default:
			if i+1 < len(q) {
				two := q[i : i+2]
				if two == "<=" || two == ">=" || two == "!=" || two == "<>" {
					out = append(out, tok{kind: "sym", text: two})
					i += 2
					continue
				}
			}
			out = append(out, tok{kind: "sym", text: string(c)})
			i++
		}
	}
	return out, nil
}

type parser struct {
	toks []tok
	pos  int
}

func (p *parser) peek() tok {
	if p.pos < len(p.toks) {
		return p.toks[p.pos]
	}
	return tok{kind: "eof"}
}

func (p *parser) next() tok {
	t := p.peek()
	p.pos++
	return t
}

func (p *parser) is(word string) bool {
	t := p.peek()
	return (t.kind == "ident" || t.kind == "sym") && strings.EqualFold(t.text, word)
}

func (p *parser) accept(word string) bool {
	if p.is(word) {
		p.pos++
		return true
	}
	return false
}

func (p *parser) expect(word string) error {
	if !p.accept(word) {
		return fmt.Errorf("esperado %s perto de %q", word, p.peek().text)
	}
	return nil
}

func (p *parser) literal() (any, error) {
	t := p.next()
	switch {
	case t.kind == "num" || t.kind == "str":
		return t.val, nil
	case t.kind == "ident" && strings.EqualFold(t.text, "NULL"):
		return nil, nil
	case t.kind == "ident" && strings.EqualFold(t.text, "TRUE"):
		return true, nil
	case t.kind == "ident" && strings.EqualFold(t.text, "FALSE"):
		return false, nil
	}
	return nil, fmt.Errorf("valor inválido %q", t.text)
}

type cond struct {
	col string
	op  string
	val any
}

func (p *parser) where() ([]cond, error) {
	var out []cond
	if !p.accept("WHERE") {
		return nil, nil
	}
	for {
		col := p.next()
		if col.kind != "ident" {
			return nil, errors.New("condição inválida")
		}
		op := p.next()
		val, err := p.literal()
		if err != nil {
			return nil, err
		}
		out = append(out, cond{col: col.text, op: op.text, val: val})
		if !p.accept("AND") {
			return out, nil
		}
	}
}

func num(v any) (float64, bool) {
	switch x := v.(type) {
	case int64:
		return float64(x), true
	case float64:
		return x, true
	}
	return 0, false
}

func compare(a, b any) int {
	if fa, ok := num(a); ok {
		if fb, ok := num(b); ok {
			switch {
			case fa < fb:
				return -1
			case fa > fb:
				return 1
			}
			return 0
		}
	}
	sa, sb := fmt.Sprint(a), fmt.Sprint(b)
	return strings.Compare(sa, sb)
}

func matches(row map[string]any, conds []cond) bool {
	for _, c := range conds {
		v := row[c.col]
		if v == nil || c.val == nil {
			return false
		}
		r := compare(v, c.val)
		okc := false
		switch c.op {
		case "=":
			okc = r == 0
		case "!=", "<>":
			okc = r != 0
		case "<":
			okc = r < 0
		case "<=":
			okc = r <= 0
		case ">":
			okc = r > 0
		case ">=":
			okc = r >= 0
		}
		if !okc {
			return false
		}
	}
	return true
}

func coerce(typ string, v any) any {
	if v == nil {
		return nil
	}
	switch typ {
	case "REAL":
		if f, ok := num(v); ok {
			return f
		}
	case "INT":
		if f, ok := num(v); ok {
			return int64(f)
		}
		if b, ok := v.(bool); ok {
			if b {
				return int64(1)
			}
			return int64(0)
		}
	}
	return v
}

func cmdResult(tag string, n int) resp.Value {
	return jsonv(map[string]any{"kind": "command", "tag": tag, "affected": n, "ms": 0.1})
}

func (f *fakeArca) sql(q string) resp.Value {
	toks, err := lex(q)
	if err != nil {
		return errv(err.Error())
	}
	p := &parser{toks: toks}
	var v resp.Value
	switch {
	case p.accept("CREATE"):
		if p.accept("TABLE") {
			v, err = f.createTable(p)
		} else if p.accept("INDEX") {
			v, err = f.createIndex(p)
		} else {
			err = errors.New("CREATE não suportado")
		}
	case p.accept("INSERT"):
		v, err = f.insert(p)
	case p.accept("SELECT"):
		v, err = f.selectRows(p)
	case p.accept("UPDATE"):
		v, err = f.update(p)
	case p.accept("DELETE"):
		v, err = f.deleteRows(p)
	default:
		err = errors.New("instrução não suportada pelo servidor falso")
	}
	if err != nil {
		return errv(err.Error())
	}
	return v
}

func (f *fakeArca) table(name string) (*fakeTable, error) {
	t, ok := f.tables[name]
	if !ok {
		return nil, fmt.Errorf("tabela '%s' não existe", name)
	}
	return t, nil
}

func (f *fakeArca) createTable(p *parser) (resp.Value, error) {
	ifNot := false
	if p.accept("IF") {
		p.expect("NOT")
		p.expect("EXISTS")
		ifNot = true
	}
	name := p.next().text
	if _, ok := f.tables[name]; ok {
		if ifNot {
			return cmdResult("CREATE TABLE", 0), nil
		}
		return resp.Value{}, fmt.Errorf("tabela '%s' já existe", name)
	}
	if err := p.expect("("); err != nil {
		return resp.Value{}, err
	}
	t := &fakeTable{name: name}
	for {
		c := fakeCol{name: p.next().text, typ: strings.ToUpper(p.next().text)}
		for !p.is(",") && !p.is(")") {
			switch {
			case p.accept("PRIMARY"):
				p.expect("KEY")
				c.pk = true
			case p.accept("AUTOINCREMENT"):
				c.auto = true
			case p.accept("NOT"):
				p.expect("NULL")
				c.notNull = true
			case p.accept("DEFAULT"):
				d, err := p.literal()
				if err != nil {
					return resp.Value{}, err
				}
				c.def = d
			default:
				return resp.Value{}, fmt.Errorf("modificador inválido %q", p.peek().text)
			}
		}
		t.cols = append(t.cols, c)
		if p.accept(")") {
			break
		}
		p.expect(",")
	}
	f.tables[name] = t
	return cmdResult("CREATE TABLE", 0), nil
}

func (f *fakeArca) createIndex(p *parser) (resp.Value, error) {
	if p.accept("IF") {
		p.expect("NOT")
		p.expect("EXISTS")
	}
	name := p.next().text
	p.expect("ON")
	t, err := f.table(p.next().text)
	if err != nil {
		return resp.Value{}, err
	}
	p.expect("(")
	col := p.next().text
	p.expect(")")
	for _, i := range t.indexes {
		if i.name == name {
			return cmdResult("CREATE INDEX", 0), nil
		}
	}
	t.indexes = append(t.indexes, fakeIndex{name: name, column: col})
	return cmdResult("CREATE INDEX", 0), nil
}

func (f *fakeArca) insert(p *parser) (resp.Value, error) {
	p.expect("INTO")
	t, err := f.table(p.next().text)
	if err != nil {
		return resp.Value{}, err
	}
	var cols []string
	if p.accept("(") {
		for {
			cols = append(cols, p.next().text)
			if p.accept(")") {
				break
			}
			p.expect(",")
		}
	} else {
		for _, c := range t.cols {
			cols = append(cols, c.name)
		}
	}
	if err := p.expect("VALUES"); err != nil {
		return resp.Value{}, err
	}
	n := 0
	for {
		p.expect("(")
		row := map[string]any{}
		for i := 0; ; i++ {
			v, err := p.literal()
			if err != nil {
				return resp.Value{}, err
			}
			if i < len(cols) {
				row[cols[i]] = v
			}
			if p.accept(")") {
				break
			}
			p.expect(",")
		}
		for _, c := range t.cols {
			v, present := row[c.name]
			if !present || v == nil {
				if c.auto {
					t.autoinc++
					v = t.autoinc
				} else if c.def != nil {
					v = c.def
				}
			}
			v = coerce(c.typ, v)
			if v == nil && (c.notNull || c.pk) {
				return resp.Value{}, fmt.Errorf("a coluna '%s' não pode ser nula", c.name)
			}
			if c.pk {
				for _, existing := range t.rows {
					if compare(existing[c.name], v) == 0 {
						return resp.Value{}, fmt.Errorf("chave primária duplicada em '%s'", t.name)
					}
				}
				if iv, ok := v.(int64); ok && iv > t.autoinc {
					t.autoinc = iv
				}
			}
			row[c.name] = v
		}
		t.rows = append(t.rows, row)
		n++
		if !p.accept(",") {
			break
		}
	}
	return cmdResult(fmt.Sprintf("INSERT %d", n), n), nil
}

func (f *fakeArca) selectRows(p *parser) (resp.Value, error) {
	var proj []string
	count := false
	if p.accept("*") {
	} else if p.accept("COUNT") {
		p.expect("(")
		p.expect("*")
		p.expect(")")
		count = true
	} else {
		for {
			proj = append(proj, p.next().text)
			if !p.accept(",") {
				break
			}
		}
	}
	if err := p.expect("FROM"); err != nil {
		return resp.Value{}, err
	}
	t, err := f.table(p.next().text)
	if err != nil {
		return resp.Value{}, err
	}
	conds, err := f.whereFor(p)
	if err != nil {
		return resp.Value{}, err
	}
	var rows []map[string]any
	for _, r := range t.rows {
		if matches(r, conds) {
			rows = append(rows, r)
		}
	}
	if count {
		return jsonv(map[string]any{"kind": "rows", "columns": []string{"COUNT(*)"}, "types": []string{"INT"}, "rows": [][]any{{len(rows)}}, "plan": "scan " + t.name, "ms": 0.1}), nil
	}
	if p.accept("ORDER") {
		p.expect("BY")
		col := p.next().text
		desc := p.accept("DESC")
		if !desc {
			p.accept("ASC")
		}
		sort.SliceStable(rows, func(i, j int) bool {
			c := compare(rows[i][col], rows[j][col])
			if desc {
				return c > 0
			}
			return c < 0
		})
	}
	limit, offset := len(rows), 0
	if p.accept("LIMIT") {
		v, _ := p.literal()
		limit = int(v.(int64))
	}
	if p.accept("OFFSET") {
		v, _ := p.literal()
		offset = int(v.(int64))
	}
	if p.peek().kind != "eof" {
		return resp.Value{}, fmt.Errorf("sintaxe inválida perto de %q", p.peek().text)
	}
	if proj == nil {
		for _, c := range t.cols {
			proj = append(proj, c.name)
		}
	}
	types := make([]string, len(proj))
	for i, name := range proj {
		for _, c := range t.cols {
			if c.name == name {
				types[i] = c.typ
			}
		}
	}
	out := [][]any{}
	for i := offset; i < len(rows) && len(out) < limit; i++ {
		line := make([]any, len(proj))
		for j, name := range proj {
			line[j] = rows[i][name]
		}
		out = append(out, line)
	}
	return jsonv(map[string]any{"kind": "rows", "columns": proj, "types": types, "rows": out, "plan": "scan " + t.name, "ms": 0.2}), nil
}

func (f *fakeArca) whereFor(p *parser) ([]cond, error) {
	return p.where()
}

func (f *fakeArca) update(p *parser) (resp.Value, error) {
	t, err := f.table(p.next().text)
	if err != nil {
		return resp.Value{}, err
	}
	p.expect("SET")
	set := map[string]any{}
	for {
		col := p.next().text
		p.expect("=")
		v, err := p.literal()
		if err != nil {
			return resp.Value{}, err
		}
		set[col] = v
		if !p.accept(",") {
			break
		}
	}
	conds, err := p.where()
	if err != nil {
		return resp.Value{}, err
	}
	n := 0
	for _, r := range t.rows {
		if matches(r, conds) {
			for k, v := range set {
				for _, c := range t.cols {
					if c.name == k {
						r[k] = coerce(c.typ, v)
					}
				}
			}
			n++
		}
	}
	return cmdResult(fmt.Sprintf("UPDATE %d", n), n), nil
}

func (f *fakeArca) deleteRows(p *parser) (resp.Value, error) {
	p.expect("FROM")
	t, err := f.table(p.next().text)
	if err != nil {
		return resp.Value{}, err
	}
	conds, err := p.where()
	if err != nil {
		return resp.Value{}, err
	}
	kept := t.rows[:0]
	n := 0
	for _, r := range t.rows {
		if matches(r, conds) {
			n++
			continue
		}
		kept = append(kept, r)
	}
	t.rows = kept
	return cmdResult(fmt.Sprintf("DELETE %d", n), n), nil
}

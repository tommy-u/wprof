#include "strs.h"
#include "wrust.h"

/*
 * LOW-LEVEL JSON BUILDER PRIMITIVES
 *
 * All functions take a struct json_state* as the first argument.
 * Track nesting level, scope, and comma state for correct output.
 * Outputs one JSON object per line (newline-delimited JSON).
 */

enum json_scope {
	JSON_OBJ,
	JSON_ARR,
};

/* Batches contain ASCII syntax and complete typed tokens with native-endian
 * payloads. Strings are copied immediately, including their NUL terminator;
 * float format pointers refer to static literals.
 */
enum json_token {
	JSON_TOKEN_INT = 0x80,
	JSON_TOKEN_UINT = 0x81,
	JSON_TOKEN_FLOAT = 0x82,
	JSON_TOKEN_TS = 0x83,
	JSON_TOKEN_STRING = 0x84,
	JSON_TOKEN_KEY = 0x85,
	JSON_TOKEN_KEY_COMMA = 0x86,
	JSON_TOKEN_QUOTED = 0x87,
	JSON_TOKEN_RAW = 0x90,
	JSON_TOKEN_EMIT = 0x88,
};

struct json_state {
	FILE *f;
	struct sbuf out;
	struct wpool *pool;
	void *emit_ctx;
	char *batch;
	size_t batch_len;
	size_t batch_capacity;
	int lvl;
	enum json_scope scope[5];
	int cnt[5]; /* comma tracking per level */
};

typedef void (*json_emit_fn)(struct json_state *js, void *ctx, const void *event, const void *snapshot);

struct json_emit_record {
	json_emit_fn emit;
	const void *event;
	size_t snapshot_len;
};

struct json_job {
	char *batch;
	size_t batch_len;
	void *emit_ctx;
	struct sbuf out;
};

#define JSON_STATE_INIT(file) { .f = (file), .lvl = -1 }
#define JSON_BUF_SZ (1024 * 1024)

static inline void json_frag_start(struct json_state *js)
{
	/* Keep fragments nested so closing an object does not flush an NDJSON record. */
	*js = (struct json_state){ .lvl = 0 };
}

static inline struct sbuf json_frag_end(struct json_state *js)
{
	struct sbuf out = js->out;

	js->out = sbuf_new();
	return out;
}

static inline void json_flush(struct json_state *js)
{
	fwrite(js->out.buf, 1, js->out.len, js->f);
	sbuf_reset(&js->out);
}

static inline void json_write(struct json_state *js, const void *p, size_t n)
{
	sbuf_append(&js->out, p, n);
}

static inline void json_fragment(struct json_state *js, const void *data, size_t len);

static inline void json_raw(struct json_state *js, const struct sbuf *fragment)
{
	json_fragment(js, fragment->buf, fragment->len);
}

static inline void json_putc(struct json_state *js, char c)
{
	sbuf_putc(&js->out, c);
}

/* Write a NUL-terminated decimal string backward from the buffer's end; return its start. */
static inline char *json_decimal(char *buf, size_t size, unsigned long long value)
{
	char *end = buf + size - 1;

	*end = '\0';
	do {
		--end;
		*end = '0' + value % 10;
		value /= 10;
	} while (value);
	return end;
}

static inline void json_int(struct json_state *js, long long value)
{
	/* 19 decimal digits + sign + NUL. */
	char buf[21], *end = buf + sizeof(buf) - 1;
	unsigned long long magnitude = value;

	if (value < 0)
		magnitude = 0ULL - magnitude; /* unsigned subtraction also handles LLONG_MIN */
	char *p = json_decimal(buf, sizeof(buf), magnitude);
	if (value < 0) {
		--p;
		*p = '-';
	}
	json_write(js, p, end - p);
}

static inline void json_uint(struct json_state *js, unsigned long long value)
{
	/* 20 decimal digits + NUL. */
	char buf[21], *end = buf + sizeof(buf) - 1;
	char *p = json_decimal(buf, sizeof(buf), value);

	json_write(js, p, end - p);
}

/*
 * Write value exactly as printf("%.6lf") would in the C locale with the default
 * rounding mode. Return false without writing if value is not finite or
 * abs(value) >= 2^43. This conservative bound keeps millionths in a u64 and
 * signed output within the 21-byte buffer.
 */
static inline bool json_fixed6(struct json_state *js, double value)
{
	u64 bits;

	memcpy(&bits, &value, sizeof(bits));
	unsigned exponent = (bits >> 52) & 0x7ff; /* biased by 1023; 0x7ff is inf/NaN */
	if (exponent >= 1023 + 43)
		return false;

	/*
	 * printf rounds the exact binary value, so llround(value * 1e6) can differ
	 * near ties. Count millionths exactly in integers instead, rounding ties to
	 * even like printf. Anything below 2^-21 (including zero and subnormals) is
	 * under half a millionth and rounds to zero.
	 */
	u64 units = 0;

	if (exponent >= 1023 - 21) {
		/* abs(value) == significand / 2^shift exactly, with shift in [10, 73] */
		u64 significand = (bits & ((1ULL << 52) - 1)) | (1ULL << 52);
		int shift = 1023 + 52 - exponent;
		__uint128_t scaled = (__uint128_t)significand * 1000000;
		__uint128_t rem = scaled & (((__uint128_t)1 << shift) - 1);
		__uint128_t half = (__uint128_t)1 << (shift - 1);

		units = scaled >> shift;
		if (rem > half || (rem == half && (units & 1)))
			units++;
	}

	/* sign + 13 whole digits (< 2^43) + '.' + 6 fraction digits, written backward */
	u64 whole = units / 1000000, frac = units % 1000000;
	char buf[21], *end = buf + sizeof(buf), *p = end;

	for (int i = 0; i < 6; i++) {
		--p;
		*p = '0' + frac % 10;
		frac /= 10;
	}
	--p;
	*p = '.';
	do {
		--p;
		*p = '0' + whole % 10;
		whole /= 10;
	} while (whole);
	/* Like printf, keep the sign of -0.0 and of negatives that round to zero. */
	if (bits >> 63) {
		--p;
		*p = '-';
	}
	json_write(js, p, end - p);
	return true;
}

static inline void json_float(struct json_state *js, const char *fmt, double value)
{
	char buf[32];

	if (strcmp(fmt, "%.6lf") == 0 && json_fixed6(js, value))
		return;

	int n = snprintf(buf, sizeof(buf), fmt, value);

	if (n < 0)
		BUG("JSON float formatting failed for format '%s'\n", fmt);
	if ((size_t)n >= sizeof(buf))
		BUG("JSON float buffer too small for format '%s': need %zu bytes, have %zu\n",
		    fmt, (size_t)n + 1, sizeof(buf));
	json_write(js, buf, n);
}

static inline void json_ts(struct json_state *js, u64 ns)
{
	static const u64 ns_per_sec = 1000000000ULL;
	char frac[] = ".000000000";

	json_uint(js, ns / ns_per_sec);
	json_decimal(frac + 1, sizeof(frac) - 1, ns % ns_per_sec);
	json_write(js, frac, sizeof(frac) - 1);
}

static inline void json_escape(struct json_state *js, unsigned char c)
{
	static const char hex[] = "0123456789abcdef";
	char buf[] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };

	json_write(js, buf, sizeof(buf));
}

/*
 * Length of the well-formed UTF-8 sequence starting at p, or 0 if p[0] is not a
 * valid lead byte / the sequence is malformed (overlong, surrogate, truncated).
 * A NUL terminator can never pass as a continuation byte, so this never reads
 * past the end of a C string.
 */
static inline int json_utf8_seq_len(const unsigned char *p)
{
	switch (p[0]) {
	case 0x00 ... 0x7f:
		return 1;
	case 0xc2 ... 0xdf:
		return (p[1] & 0xc0) == 0x80 ? 2 : 0;
	case 0xe0:
		return (p[1] >= 0xa0 && p[1] <= 0xbf && (p[2] & 0xc0) == 0x80) ? 3 : 0;
	case 0xe1 ... 0xec:
	case 0xee ... 0xef:
		return ((p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) ? 3 : 0;
	case 0xed:
		return (p[1] >= 0x80 && p[1] <= 0x9f && (p[2] & 0xc0) == 0x80) ? 3 : 0;
	case 0xf0:
		return (p[1] >= 0x90 && p[1] <= 0xbf && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) ? 4 : 0;
	case 0xf1 ... 0xf3:
		return ((p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) ? 4 : 0;
	case 0xf4:
		return (p[1] >= 0x80 && p[1] <= 0x8f && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) ? 4 : 0;
	default:
		return 0;
	}
}

/*
 * Write s as the body of a JSON string (caller emits the surrounding quotes),
 * escaping per RFC 8259. Control characters become \uXXXX; each byte of an
 * invalid UTF-8 sequence is escaped as \u00XX, preserving its value, so
 * arbitrary byte content (e.g. utrace buffers or metadata with quotes or
 * newlines) can never produce undecodable output.
 */
static inline const char *json_puts(struct json_state *js, const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	while (*p) {
		const unsigned char *run = p;

		while (*p >= 0x20 && *p < 0x80 && *p != '"' && *p != '\\')
			p++;
		if (p > run)
			json_write(js, run, p - run);

		unsigned char c = *p;
		switch (c) {
		case 0: return (const char *)p;
		case '"':  json_write(js, "\\\"", 2); p++; break;
		case '\\': json_write(js, "\\\\", 2); p++; break;
		case '\n': json_write(js, "\\n", 2); p++; break;
		case '\t': json_write(js, "\\t", 2); p++; break;
		case '\r': json_write(js, "\\r", 2); p++; break;
		case '\b': json_write(js, "\\b", 2); p++; break;
		case '\f': json_write(js, "\\f", 2); p++; break;
		default:
			if (c < 0x20) {
				json_escape(js, c);
				p++;
			} else {
				int n = json_utf8_seq_len(p);

				if (n) {
					json_write(js, p, n);
					p += n;
				} else {
					json_escape(js, c);
					p++;
				}
			}
		}
	}
	return (const char *)p;
}

static inline const char *json_putquoted(struct json_state *js, const char *s)
{
	json_putc(js, '"');
	const char *end = json_puts(js, s);
	json_putc(js, '"');
	return end;
}

static inline const char *json_putkey(struct json_state *js, const char *key, bool comma)
{
	if (comma)
		json_putc(js, ',');
	json_putc(js, '"');
	const char *end = json_puts(js, key);
	json_write(js, "\":", 2);
	return end;
}

/* Payloads are native-endian and unaligned; copied strings include their NUL terminator. */
static inline void json_render(struct json_state *js, const char *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	const unsigned char *end = p + len;

	sbuf_ensure(&js->out, JSON_BUF_SZ);
	while (p < end) {
		unsigned char token = *p;
		p++;

		switch (token) {
		case JSON_TOKEN_INT: {
			long long value;

			memcpy(&value, p, sizeof(value));
			p += sizeof(value);
			json_int(js, value);
			break;
		}
		case JSON_TOKEN_UINT: {
			unsigned long long value;

			memcpy(&value, p, sizeof(value));
			p += sizeof(value);
			json_uint(js, value);
			break;
		}
		case JSON_TOKEN_FLOAT: {
			const char *fmt;
			double value;

			memcpy(&fmt, p, sizeof(fmt));
			p += sizeof(fmt);
			memcpy(&value, p, sizeof(value));
			p += sizeof(value);
			json_float(js, fmt, value);
			break;
		}
		case JSON_TOKEN_TS: {
			u64 ns;

			memcpy(&ns, p, sizeof(ns));
			p += sizeof(ns);
			json_ts(js, ns);
			break;
		}
		case JSON_TOKEN_EMIT: {
			struct json_emit_record record;

			memcpy(&record, p, sizeof(record));
			p += sizeof(record);
			record.emit(js, js->emit_ctx, record.event, p);
			p += record.snapshot_len;
			break;
		}
		case JSON_TOKEN_STRING:
			p = (const unsigned char *)json_puts(js, (const char *)p) + 1;
			break;
		case JSON_TOKEN_KEY:
		case JSON_TOKEN_KEY_COMMA:
			p = (const unsigned char *)json_putkey(js, (const char *)p, token == JSON_TOKEN_KEY_COMMA) + 1;
			break;
		case JSON_TOKEN_QUOTED:
			p = (const unsigned char *)json_putquoted(js, (const char *)p) + 1;
			break;
		case JSON_TOKEN_RAW: {
			size_t n;

			memcpy(&n, p, sizeof(n));
			p += sizeof(n);
			json_write(js, p, n);
			p += n;
			break;
		}
		default:
			json_putc(js, token);
		}
	}
}

static inline void json_work(void *ctx, void *ptr)
{
	struct json_job *job = ptr;

	(void)ctx;
	struct json_state render = JSON_STATE_INIT(NULL);

	render.emit_ctx = job->emit_ctx;
	json_render(&render, job->batch, job->batch_len);
	job->out = render.out;
}

static inline void json_done(void *ctx, void *ptr)
{
	struct json_job *job = ptr;
	FILE *f = ctx;

	fwrite(job->out.buf, 1, job->out.len, f);
	free(job->batch);
	sbuf_free(&job->out);
	free(job);
}

static inline void json_submit(struct json_state *js)
{
	if (!js->batch_len)
		return;
	struct json_job *job = malloc(sizeof(*job));

	*job = (struct json_job) { .batch = js->batch, .batch_len = js->batch_len, .emit_ctx = js->emit_ctx };
	js->batch = NULL;
	js->batch_len = 0;
	js->batch_capacity = 0;
	wpool_submit(js->pool, job);
}

static inline char *json_token_reserve(struct json_state *js, size_t n)
{
	if (n > js->batch_capacity - js->batch_len)
		json_submit(js);
	if (n > js->batch_capacity) {
		size_t capacity = n > JSON_BUF_SZ ? n : JSON_BUF_SZ;

		js->batch = realloc(js->batch, capacity);
		js->batch_capacity = capacity;
	}
	char *p = js->batch + js->batch_len;

	js->batch_len += n;
	return p;
}

static inline void json_token_data(struct json_state *js, enum json_token token, const void *data, size_t n)
{
	char *p = json_token_reserve(js, 1 + n);

	*p = token;
	p++;
	memcpy(p, data, n);
}

/* Cached fragments may contain UTF-8 bytes that overlap token IDs. */
static inline void json_fragment(struct json_state *js, const void *data, size_t n)
{
	if (js->deferred) {
		char *p = json_token_reserve(js, 1 + sizeof(n) + n);

		*p = JSON_TOKEN_RAW;
		p++;
		memcpy(p, &n, sizeof(n));
		p += sizeof(n);
		memcpy(p, data, n);
	} else {
		json_write(js, data, n);
	}
}

/* Event and context pointers remain valid through finish; snapshots are copied into the batch. */
static inline char *json_emit_reserve(struct json_state *js, json_emit_fn emit, const void *event, size_t snapshot_len)
{
	struct json_emit_record record = { .emit = emit, .event = event, .snapshot_len = snapshot_len };
	char *p = json_token_reserve(js, 1 + sizeof(record) + snapshot_len);

	*p = JSON_TOKEN_EMIT;
	p++;
	memcpy(p, &record, sizeof(record));
	return p + sizeof(record);
}

static inline void json_token_string(struct json_state *js, enum json_token token, const char *s)
{
	json_token_data(js, token, s, strlen(s) + 1);
}

static inline void json_literal(struct json_state *js, const char *s, size_t n)
{
	if (js->pool)
		memcpy(json_token_reserve(js, n), s, n);
	else
		json_write(js, s, n);
}

static inline void json_char(struct json_state *js, char c)
{
	if (js->pool)
		*json_token_reserve(js, 1) = c;
	else
		json_putc(js, c);
}

static inline void json_string(struct json_state *js, const char *s, bool quoted)
{
	if (js->pool)
		json_token_string(js, quoted ? JSON_TOKEN_QUOTED : JSON_TOKEN_STRING, s);
	else if (quoted)
		json_putquoted(js, s);
	else
		json_puts(js, s);
}

static inline void json_value_int(struct json_state *js, long long value)
{
	if (js->pool)
		json_token_data(js, JSON_TOKEN_INT, &value, sizeof(value));
	else
		json_int(js, value);
}

static inline void json_value_uint(struct json_state *js, unsigned long long value)
{
	if (js->pool)
		json_token_data(js, JSON_TOKEN_UINT, &value, sizeof(value));
	else
		json_uint(js, value);
}

static inline void json_value_float(struct json_state *js, const char *fmt, double value)
{
	if (js->pool) {
		char *p = json_token_reserve(js, 1 + sizeof(fmt) + sizeof(value));

		*p = JSON_TOKEN_FLOAT;
		p++;
		memcpy(p, &fmt, sizeof(fmt));
		memcpy(p + sizeof(fmt), &value, sizeof(value));
	} else {
		json_float(js, fmt, value);
	}
}

static inline void json_value_ts(struct json_state *js, u64 ns)
{
	if (js->pool)
		json_token_data(js, JSON_TOKEN_TS, &ns, sizeof(ns));
	else
		json_ts(js, ns);
}

static inline void json_init(struct json_state *js)
{
	js->pool = wpool_new(4, json_work, json_done, js->f);
	if (js->pool) {
		js->batch = malloc(JSON_BUF_SZ);
		js->batch_capacity = JSON_BUF_SZ;
	}
}

static inline void json_finish(struct json_state *js)
{
	if (js->pool) {
		json_submit(js);
		wpool_finish(js->pool);
		free(js->batch);
	} else if (js->out.len) {
		json_flush(js);
	}
	sbuf_free(&js->out);
}

static inline void json_str(struct json_state *js, const char *value)
{
	json_string(js, value, true);
}

static inline void json_bool(struct json_state *js, bool value)
{
	json_literal(js, value ? "true" : "false", value ? 4 : 5);
}

static inline void json_null(struct json_state *js)
{
	json_literal(js, "null", 4);
}

static inline void json_vprintf(struct json_state *js, const char *fmt, va_list ap)
{
	char buf[256];
	va_list ap2;

	va_copy(ap2, ap);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	if (n < 0) {
		va_end(ap2);
		return;
	}
	if ((size_t)n < sizeof(buf)) {
		json_string(js, buf, false);
	} else {
		char *heap = malloc(n + 1);

		vsnprintf(heap, n + 1, fmt, ap2);
		json_string(js, heap, false);
		free(heap);
	}
	va_end(ap2);
}

__attribute__((format(printf, 2, 3)))
static inline void json_fmt(struct json_state *js, const char *fmt, ...)
{
	json_char(js, '"');

	va_list ap;
	va_start(ap, fmt);
	json_vprintf(js, fmt, ap);
	va_end(ap);

	json_char(js, '"');
}

static inline void json_obj_start(struct json_state *js)
{
	++js->lvl;
	js->scope[js->lvl] = JSON_OBJ;
	js->cnt[js->lvl] = 0;
	json_char(js, '{');
}

static inline void json_obj_end(struct json_state *js)
{
	js->cnt[js->lvl] = 0;
	js->lvl--;
	if (js->lvl < 0) { /* outermost level, emit newline */
		json_literal(js, "}\n", 2);
		if (!js->pool && js->f)
			json_flush(js);
	} else
		json_char(js, '}');
}

static inline void json_key(struct json_state *js, const char *key)
{
	if (js->pool)
		json_token_string(js, js->cnt[js->lvl] ? JSON_TOKEN_KEY_COMMA : JSON_TOKEN_KEY, key);
	else
		json_putkey(js, key, js->cnt[js->lvl]);
	js->cnt[js->lvl]++;
}

static inline void json_literal_field(struct json_state *js, const char *text, size_t len)
{
	if (js->cnt[js->lvl])
		json_char(js, ',');
	json_literal(js, text, len);
	js->cnt[js->lvl]++;
}

static inline void json_kv_str(struct json_state *js, const char *key, const char *value)
{
	json_key(js, key);
	json_string(js, value, true);
}

__attribute__((format(printf, 3, 4)))
static inline void json_kv_fmt(struct json_state *js, const char *key, const char *fmt, ...)
{
	json_key(js, key);
	json_char(js, '"');

	va_list ap;
	va_start(ap, fmt);
	json_vprintf(js, fmt, ap);
	va_end(ap);

	json_char(js, '"');
}

static inline void json_kv_int(struct json_state *js, const char *key, long long value)
{
	json_key(js, key);
	json_value_int(js, value);
}

static inline void json_kv_uint(struct json_state *js, const char *key, unsigned long long value)
{
	json_key(js, key);
	json_value_uint(js, value);
}

static inline void json_kv_null(struct json_state *js, const char *key)
{
	json_key(js, key);
	json_literal(js, "null", 4);
}

static inline void json_kv_float(struct json_state *js, const char *key, const char *fmt, double value)
{
	json_key(js, key);
	json_value_float(js, fmt, value);
}

static inline void json_kv_ts(struct json_state *js, const char *key, u64 ns)
{
	json_key(js, key);
	json_value_ts(js, ns);
}

static inline void json_kv_bool(struct json_state *js, const char *key, bool value)
{
	json_key(js, key);
	json_literal(js, value ? "true" : "false", value ? 4 : 5);
}

#define json_key_literal(js, key) \
	json_literal_field(js, "\"" key "\":", sizeof("\"" key "\":") - 1)
#define json_kv_const_str(js, key, value) \
	json_literal_field(js, "\"" key "\":\"" value "\"", sizeof("\"" key "\":\"" value "\"") - 1)
#define json_kv_int_literal(js, key, value) \
	do { json_key_literal(js, key); json_value_int(js, value); } while (0)
#define json_kv_ts_literal(js, key, value) \
	do { json_key_literal(js, key); json_value_ts(js, value); } while (0)
#define json_kv_str_literal(js, key, value) \
	do { json_key_literal(js, key); json_str(js, value); } while (0)
#define json_kv_bool_literal(js, key, value) \
	do { json_key_literal(js, key); json_bool(js, value); } while (0)
#define json_kv_float_literal(js, key, fmt, value) \
	do { json_key_literal(js, key); json_value_float(js, fmt, value); } while (0)
#define json_kv_fmt_literal(js, key, ...) \
	do { json_key_literal(js, key); json_fmt(js, __VA_ARGS__); } while (0)
#define json_subarr_start_literal(js, key) \
	do { json_key_literal(js, key); json_arr_start(js); } while (0)
#define json_subobj_start_literal(js, key) \
	do { json_key_literal(js, key); json_obj_start(js); } while (0)

static inline void json_arr_start(struct json_state *js)
{
	++js->lvl;
	js->scope[js->lvl] = JSON_ARR;
	js->cnt[js->lvl] = 0;
	json_char(js, '[');
}

static inline void json_arr_end(struct json_state *js)
{
	js->cnt[js->lvl] = 0;
	js->lvl--;
	json_char(js, ']');
}

static inline void json_arr_elem(struct json_state *js)
{
	if (js->cnt[js->lvl])
		json_char(js, ',');
	js->cnt[js->lvl]++;
}

static inline void json_arr_raw(struct json_state *js, const struct sbuf *fragment)
{
	json_arr_elem(js);
	json_raw(js, fragment);
}

static inline void json_arr_str(struct json_state *js, const char *value)
{
	json_arr_elem(js);
	json_string(js, value, true);
}

__attribute__((format(printf, 2, 3)))
static inline void json_arr_fmt(struct json_state *js, const char *fmt, ...)
{
	json_arr_elem(js);
	json_char(js, '"');

	va_list ap;
	va_start(ap, fmt);
	json_vprintf(js, fmt, ap);
	va_end(ap);

	json_char(js, '"');
}

__unused
static inline void json_arr_int(struct json_state *js, long long value)
{
	json_arr_elem(js);
	json_value_int(js, value);
}

static inline void json_arr_float(struct json_state *js, const char *fmt, double value)
{
	json_arr_elem(js);
	json_value_float(js, fmt, value);
}

__unused
static inline void json_arr_null(struct json_state *js)
{
	json_arr_elem(js);
	json_literal(js, "null", 4);
}

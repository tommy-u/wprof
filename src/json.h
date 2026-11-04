#include "strs.h"

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

struct json_state {
	FILE *f;
	struct sbuf out;
	int lvl;
	enum json_scope scope[5];
	int cnt[5]; /* comma tracking per level */
};

#define JSON_STATE_INIT(file) { .f = (file), .lvl = -1 }

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

static inline void json_finish(struct json_state *js)
{
	sbuf_free(&js->out);
}

static inline void json_write(struct json_state *js, const void *p, size_t n)
{
	sbuf_append(&js->out, p, n);
}

static inline void json_raw(struct json_state *js, const struct sbuf *fragment)
{
	json_write(js, fragment->buf, fragment->len);
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
static inline void json_puts(struct json_state *js, const char *s)
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
		case 0: return;
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
}

static inline void json_str(struct json_state *js, const char *value)
{
	json_putc(js, '"');
	json_puts(js, value);
	json_putc(js, '"');
}

static inline void json_bool(struct json_state *js, bool value)
{
	json_write(js, value ? "true" : "false", value ? 4 : 5);
}

static inline void json_null(struct json_state *js)
{
	json_write(js, "null", 4);
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
		json_puts(js, buf);
	} else {
		char *heap = malloc(n + 1);

		vsnprintf(heap, n + 1, fmt, ap2);
		json_puts(js, heap);
		free(heap);
	}
	va_end(ap2);
}

__attribute__((format(printf, 2, 3)))
static inline void json_fmt(struct json_state *js, const char *fmt, ...)
{
	json_putc(js, '"');

	va_list ap;
	va_start(ap, fmt);
	json_vprintf(js, fmt, ap);
	va_end(ap);

	json_putc(js, '"');
}

static inline void json_obj_start(struct json_state *js)
{
	++js->lvl;
	js->scope[js->lvl] = JSON_OBJ;
	js->cnt[js->lvl] = 0;
	json_putc(js, '{');
}

static inline void json_obj_end(struct json_state *js)
{
	js->cnt[js->lvl] = 0;
	js->lvl--;
	if (js->lvl < 0) { /* outermost level, emit newline */
		json_write(js, "}\n", 2);
		json_flush(js);
	} else
		json_putc(js, '}');
}

static inline void json_key(struct json_state *js, const char *key)
{
	if (js->cnt[js->lvl])
		json_putc(js, ',');
	json_putc(js, '"');
	json_puts(js, key);
	json_write(js, "\":", 2);
	js->cnt[js->lvl]++;
}

static inline void json_literal_field(struct json_state *js, const char *text, size_t len)
{
	if (js->cnt[js->lvl])
		json_putc(js, ',');
	json_write(js, text, len);
	js->cnt[js->lvl]++;
}

#define json_key_literal(js, key) \
	json_literal_field(js, "\"" key "\":", sizeof("\"" key "\":") - 1)
#define json_kv_const_str(js, key, value) \
	json_literal_field(js, "\"" key "\":\"" value "\"", sizeof("\"" key "\":\"" value "\"") - 1)
#define json_kv_int(js, key, value) \
	do { json_key_literal(js, key); json_int(js, value); } while (0)
#define json_kv_ts(js, key, value) \
	do { json_key_literal(js, key); json_ts(js, value); } while (0)
#define json_kv_str(js, key, value) \
	do { json_key_literal(js, key); json_str(js, value); } while (0)
#define json_kv_bool(js, key, value) \
	do { json_key_literal(js, key); json_bool(js, value); } while (0)
#define json_kv_float(js, key, fmt, value) \
	do { json_key_literal(js, key); json_float(js, fmt, value); } while (0)
#define json_kv_fmt(js, key, ...) \
	do { json_key_literal(js, key); json_fmt(js, __VA_ARGS__); } while (0)
#define json_subarr_start(js, key) \
	do { json_key_literal(js, key); json_arr_start(js); } while (0)
#define json_subobj_start(js, key) \
	do { json_key_literal(js, key); json_obj_start(js); } while (0)

static inline void json_arr_start(struct json_state *js)
{
	++js->lvl;
	js->scope[js->lvl] = JSON_ARR;
	js->cnt[js->lvl] = 0;
	json_putc(js, '[');
}

static inline void json_arr_end(struct json_state *js)
{
	js->cnt[js->lvl] = 0;
	js->lvl--;
	json_putc(js, ']');
}

static inline void json_arr_elem(struct json_state *js)
{
	if (js->cnt[js->lvl])
		json_putc(js, ',');
	js->cnt[js->lvl]++;
}

static inline void json_arr_str(struct json_state *js, const char *value)
{
	json_arr_elem(js);
	json_str(js, value);
}

__attribute__((format(printf, 2, 3)))
static inline void json_arr_fmt(struct json_state *js, const char *fmt, ...)
{
	json_arr_elem(js);
	json_putc(js, '"');

	va_list ap;
	va_start(ap, fmt);
	json_vprintf(js, fmt, ap);
	va_end(ap);

	json_putc(js, '"');
}

__unused
static inline void json_arr_int(struct json_state *js, long long value)
{
	json_arr_elem(js);
	json_int(js, value);
}

static inline void json_arr_float(struct json_state *js, const char *fmt, double value)
{
	json_arr_elem(js);
	json_float(js, fmt, value);
}

__unused
static inline void json_arr_null(struct json_state *js)
{
	json_arr_elem(js);
	json_null(js);
}

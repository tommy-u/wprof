#include <pthread.h>
#include <sys/prctl.h>

#include "strs.h"
#include "wrust.h"

#define JSON_FLUSH_SZ (1024 * 1024)

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "wrust.h"

/*
 * LOW-LEVEL JSON BUILDER PRIMITIVES
 *
 * Builder helpers track nesting, scope, and comma state in json_state.
 * Formatting primitives use json_state and its sbuf storage.
 * Outputs one JSON object per line (newline-delimited JSON).
 */

enum json_scope {
	JSON_OBJ,
	JSON_ARR,
};

#define JSON_SCHEMA_KEYS(X) \
	X(ACTION, "action") \
	X(ANNS, "anns") \
	X(ARGS, "args") \
	X(BLOCK, "block") \
	X(BYTE_CNT, "byte_cnt") \
	X(CHILD, "child") \
	X(COMM, "comm") \
	X(COMPOUND_CHAIN_LEN, "compound_chain_len") \
	X(COMPOUND_DELAY, "compound_delay") \
	X(CONTEXT_ID, "context_id") \
	X(CORR_ID, "corr_id") \
	X(CPU, "cpu") \
	X(DESC, "desc") \
	X(DEVICE_ID, "device_id") \
	X(DSQ_ID, "dsq_id") \
	X(DST_DEVICE_ID, "dst_device_id") \
	X(DST_KIND, "dst_kind") \
	X(DUR, "dur") \
	X(EVENT, "event") \
	X(EXTRAS, "extras") \
	X(FILE, "file") \
	X(FILENAME, "filename") \
	X(FLOW_ID, "flow_id") \
	X(FRAMES, "frames") \
	X(GRID, "grid") \
	X(ID, "id") \
	X(INSERT_TYPE, "insert_type") \
	X(IPI_DELAY, "ipi_delay") \
	X(IRQ, "irq") \
	X(IRQ_CTX, "irq_ctx") \
	X(KIND, "kind") \
	X(LATENCY, "latency") \
	X(LAYER_ID, "layer_id") \
	X(LINENO, "lineno") \
	X(METADATA, "metadata") \
	X(METHOD, "method") \
	X(NAME, "name") \
	X(NEXT, "next") \
	X(NEXT_PRIO, "next_prio") \
	X(NEXT_STATE, "next_state") \
	X(NUMA, "numa") \
	X(OFFCPU_DUR, "offcpu_dur") \
	X(OFFCPU_STACK_ID, "offcpu_stack_id") \
	X(OLD_COMM, "old_comm") \
	X(OLD_TID, "old_tid") \
	X(PCOMM, "pcomm") \
	X(PID, "pid") \
	X(PMU, "pmu") \
	X(PMUS, "pmus") \
	X(PREV, "prev") \
	X(PREV_PRIO, "prev_prio") \
	X(PREV_STATE, "prev_state") \
	X(REQ_ID, "req_id") \
	X(REQ_NAME, "req_name") \
	X(REQ_TASK_ID, "req_task_id") \
	X(RPC_ID, "rpc_id") \
	X(SAMPLE_PERIOD, "sample_period") \
	X(SENDER_CPU, "sender_cpu") \
	X(SERVICE, "service") \
	X(SRC_DEVICE_ID, "src_device_id") \
	X(SRC_KIND, "src_kind") \
	X(SRCS, "srcs") \
	X(STACK_CNT, "stack_cnt") \
	X(STACK_ID, "stack_id") \
	X(STACKS, "stacks") \
	X(START_TS, "start_ts") \
	X(STREAM_ID, "stream_id") \
	X(SUCCESS, "success") \
	X(T, "t") \
	X(TARGET_CPU, "target_cpu") \
	X(TASK, "task") \
	X(TID, "tid") \
	X(TIMER_FREQ_HZ, "timer_freq_hz") \
	X(TIMESTAMP, "timestamp") \
	X(TS, "ts") \
	X(UTRACE_ID, "utrace_id") \
	X(VERSION, "version") \
	X(WAIT_TIME, "wait_time") \
	X(WAKEE, "wakee") \
	X(WAKER, "waker") \
	X(WAKER_STACK_ID, "waker_stack_id") \
	X(WAKING_CPU, "waking_cpu") \
	X(WAKING_REASON, "waking_reason") \
	X(WAKING_TS, "waking_ts")

#define JSON_KEY_ENUM(id, text) JSON_KEY_##id,
enum json_key_id {
	JSON_SCHEMA_KEYS(JSON_KEY_ENUM)
	JSON_KEY_COUNT,
};
#undef JSON_KEY_ENUM

_Static_assert(JSON_KEY_COUNT <= 256, "key IDs must fit in a byte");

struct json_known_key {
	const char *key;
	const char *prefix;
	size_t prefix_len;
};

#define JSON_KEY_ENTRY(id, text) [JSON_KEY_##id] = { text, "\"" text "\":", sizeof(text) + 2 },
static const struct json_known_key json_known_keys[] = {
	JSON_SCHEMA_KEYS(JSON_KEY_ENTRY)
};
#undef JSON_KEY_ENTRY
#undef JSON_SCHEMA_KEYS

/* Batches contain ASCII syntax and complete typed tokens with native-endian
 * payloads. Strings are copied immediately, including their NUL terminator;
 * float format pointers refer to static literals. Task names borrow the input
 * mapping, which stays alive through json_finish(). Combined fields contain a
 * key ID, a value kind, and its payload; FIELD_COMMA includes the leading comma.
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
	JSON_TOKEN_FIELD = 0x88,
	JSON_TOKEN_FIELD_COMMA = 0x89,
	JSON_TOKEN_NULL = 0x8a,
	JSON_TOKEN_TRUE = 0x8b,
	JSON_TOKEN_FALSE = 0x8c,
	JSON_TOKEN_KEY_ONLY = 0x8d,
	JSON_TOKEN_TASK = 0x8e,
	JSON_TOKEN_EMIT = 0x8f,
	JSON_TOKEN_RAW = 0x90,
	JSON_TOKEN_EMIT = 0x8f,
};

struct json_task_data {
	int tid;
	u32 pid;
	const char *comm;
	const char *pcomm;
};

struct json_options {
	bool no_batching;
	bool no_int_format;
	bool no_int_ts;
	bool no_key_tokens;
	bool no_literal_lengths;
	bool no_task_tokens;
};

struct json_buf {
	FILE *f;
	struct sbuf buffer;
	struct json_options options;
	u64 float_zero_count;
	u64 float_integer_count;
	u64 float_fractional_count;
	u64 float_fallback_count;
};

struct json_writer_state {
	bool started;
	bool closing;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t work;
	pthread_cond_t space;
	struct json_job *queue[2];
	size_t head;
	size_t count;
};

typedef void (*json_emit_fn)(void *ctx, const void *event, const void *snapshot, struct json_buf *out);

struct json_emit_record {
	json_emit_fn emit;
	const void *event;
	size_t snapshot_len;
};

struct json_job {
	char *batch;
	size_t batch_len;
	struct json_options options;
	void *emit_ctx;
	struct json_buf out;
	void *emit_ctx;
};

struct json_writer_state {
	bool started;
	bool closing;
	bool direct_active;
	bool nocow_active;
	bool fd_flags_changed;
	pthread_t thread;
	int fd;
	int original_fd_flags;
	int error;
	off_t logical_size;
	off_t direct_offset;
	char *direct_data;
	size_t direct_len;
	unsigned long long direct_bytes;
	unsigned long long direct_writes;
	pthread_mutex_t mutex;
	pthread_cond_t work;
	pthread_cond_t space;
	struct json_job *queue[2];
	size_t head;
	size_t count;
	u64 float_zero_count;
	u64 float_integer_count;
	u64 float_fractional_count;
	u64 float_fallback_count;
};

struct json_state {
	struct json_buf out;
	struct wpool *pool;
	struct json_writer_state writer;
	void *emit_ctx;
	char *batch;
	size_t batch_len;
	size_t batch_capacity;
	struct sbuf *task_cache;
	size_t task_cache_cnt;
	struct wpool *pool;
	struct json_writer_state writer;
	void *emit_ctx;
	bool no_parallel;
	bool no_writer;
	bool no_worker_emitters;
	bool worker_requests;
	bool direct_io;
	bool nocow;
	size_t max_workers;
	size_t batch_size;
	char *batch;
	size_t batch_len;
	size_t batch_capacity;
	size_t stack_frame_units;
	int lvl;
	enum json_scope scope[5];
	int cnt[5]; /* comma tracking per level */
	u64 float_zero_count;
	u64 float_integer_count;
	u64 float_fractional_count;
	u64 float_fallback_count;
};

static inline void json_float_stats_add(struct json_state *js, const struct json_writer_state *writer)
{
	js->float_zero_count += writer->float_zero_count;
	js->float_integer_count += writer->float_integer_count;
	js->float_fractional_count += writer->float_fractional_count;
	js->float_fallback_count += writer->float_fallback_count;
}

#define JSON_STATE_INIT(file) { .out = { .f = (file) }, .max_workers = 16, .batch_size = JSON_BUF_SZ, .lvl = -1 }

#ifndef JSON_BUF_SZ
#define JSON_BUF_SZ (1024 * 1024)
#endif

static inline void json_frag_start(struct json_state *js)
{
	/* Keep fragments nested so closing an object does not flush an NDJSON record. */
	*js = (struct json_state){ .lvl = 0 };
}

static inline struct sbuf json_frag_end(struct json_state *js)
{
	struct sbuf out = js->out.buffer;

	js->out.buffer = sbuf_new();
	return out;
}
#define JSON_DIRECT_ALIGN 4096
#define JSON_DIRECT_MEM_ALIGN (2 * 1024 * 1024)
#define JSON_DIRECT_CHUNK_SZ (4 * 1024 * 1024)

static inline void json_flush(struct json_state *js)
{
	fwrite(js->out.buffer.buf, 1, js->out.buffer.len, js->out.f);
	sbuf_reset(&js->out.buffer);
}

static inline void json_write(struct json_state *js, const void *p, size_t n)
{
	if (js->out.options.no_batching) {
			fwrite(p, 1, n, js->out.f);
			return;
	}
	sbuf_append(&js->out.buffer, p, n);
}

static inline void json_fragment(struct json_state *js, const void *data, size_t len);

static inline void json_raw(struct json_state *js, const struct sbuf *fragment)
{
	json_fragment(js, fragment->buf, fragment->len);
}

static inline void json_putc(struct json_state *js, char c)
{
	json_write(js, &c, 1);
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
	if (js->out.options.no_int_format) {
		char text[21];
		int n = snprintf(text, sizeof(text), "%lld", value);

		json_write(js, text, n);
		return;
	}
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
	if (js->out.options.no_int_format) {
		char text[21];
		int n = snprintf(text, sizeof(text), "%llu", value);

		json_write(js, text, n);
		return;
	}
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

	if (!strcmp(fmt, "%.6lf")) {
		u64 bits;

		memcpy(&bits, &value, sizeof(bits));
		u64 magnitude = bits & 0x7fffffffffffffffULL;
		if (magnitude < (1066ULL << 52)) { /* finite and abs(value) < 2^43 */
			unsigned exponent = magnitude >> 52;
			u64 significand = magnitude & ((1ULL << 52) - 1);
			int shift = 1074;

			if (exponent) {
				significand |= 1ULL << 52;
				shift = 1075 - exponent;
			}
			if (!magnitude) {
				js->float_zero_count++;
				js->out.float_zero_count++;
			} else if (shift < 64 && !(significand & ((1ULL << shift) - 1))) {
				js->float_integer_count++;
				js->out.float_integer_count++;
			} else {
				js->float_fractional_count++;
				js->out.float_fractional_count++;
			}

			/* Exact value in millionths is significand * 1000000 / 2^shift. */
			__uint128_t scaled = (__uint128_t)significand * 1000000;
			u64 units = 0;

			if (shift < 128) {
				units = scaled >> shift;
				__uint128_t remainder = scaled & (((__uint128_t)1 << shift) - 1);
				__uint128_t half = (__uint128_t)1 << (shift - 1);

				if (remainder > half || (remainder == half && (units & 1)))
					units++;
			}
			u64 whole = units / 1000000;
			u64 fraction = units % 1000000;
			/* Reserve seven bytes for the dot and six fractional digits. */
			char *p = json_decimal(buf, sizeof(buf) - 7, whole);
			char *last = buf + sizeof(buf) - 1;
			char *end = last;

			buf[sizeof(buf) - 8] = '.';
			for (int i = 0; i < 6; i++) {
				--end;
				*end = '0' + fraction % 10;
				fraction /= 10;
			}
			*last = '\0';
			if (bits >> 63) {
				--p;
				*p = '-';
			}
			json_write(js, p, last - p);
			return;
		}
		js->float_fallback_count++;
		js->out.float_fallback_count++;
	}
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
	if (js->out.options.no_int_ts) {
		json_float(js, "%.9lf", ns / 1e9);
		return;
	}
	static const u64 ns_per_sec = 1000000000ULL;
	char frac[] = ".000000000";

	json_uint(js, ns / ns_per_sec);
	json_decimal(frac + 1, sizeof(frac) - 1, ns % ns_per_sec);
	json_write(js, frac, sizeof(frac) - 1);
}

static inline void json_escape(struct json_state *js, unsigned char c)
{
	if (js->out.options.no_int_format) {
		char text[7];
		int n = snprintf(text, sizeof(text), "\\u%04x", c);

		json_write(js, text, n);
		return;
	}
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
static inline void json_render(const char *data, size_t len, struct json_options options, struct json_buf *out,
			       void *emit_ctx)
{
	const unsigned char *p = (const unsigned char *)data;
	const unsigned char *end = p + len;

	*out = (struct json_buf) { .buffer = { .buf = malloc(JSON_BUF_SZ), .cap = JSON_BUF_SZ }, .options = options };
	out->buffer.len = 0;
	struct json_state renderer = { .out = *out };

	while (p < end) {
		unsigned char token = *p++;

		if (token == JSON_TOKEN_FIELD || token == JSON_TOKEN_FIELD_COMMA) {
			unsigned char id = *p++;

			if (token == JSON_TOKEN_FIELD_COMMA)
				json_putc(&renderer, ',');
			json_write(&renderer, json_known_keys[id].prefix, json_known_keys[id].prefix_len);
			token = *p++;
		}
		switch (token) {
		case JSON_TOKEN_INT: {
			long long value;

			memcpy(&value, p, sizeof(value));
			p += sizeof(value);
			json_int(&renderer, value);
			break;
		}
		case JSON_TOKEN_UINT: {
			unsigned long long value;

			memcpy(&value, p, sizeof(value));
			p += sizeof(value);
			json_uint(&renderer, value);
			break;
		}
		case JSON_TOKEN_FLOAT: {
			const char *fmt;
			double value;

			memcpy(&fmt, p, sizeof(fmt));
			p += sizeof(fmt);
			memcpy(&value, p, sizeof(value));
			p += sizeof(value);
			json_float(&renderer, fmt, value);
			break;
		}
		case JSON_TOKEN_TS: {
			u64 ns;

			memcpy(&ns, p, sizeof(ns));
			p += sizeof(ns);
			json_ts(&renderer, ns);
			break;
		}
		case JSON_TOKEN_EMIT: {
			struct json_emit_record record;

			memcpy(&record, p, sizeof(record));
			p += sizeof(record);
			record.emit(emit_ctx, record.event, p, &renderer.out);
			p += record.snapshot_len;
			break;
		}
		case JSON_TOKEN_TASK: {
			struct json_task_data task;

			memcpy(&task, p, sizeof(task));
			p += sizeof(task);
			json_putc(&renderer, '{');
			if (task.tid) {
				json_write(&renderer, "\"tid\":", 6);
				json_int(&renderer, task.tid);
				json_putc(&renderer, ',');
			}
			json_write(&renderer, "\"pid\":", 6);
			json_int(&renderer, task.pid);
			json_write(&renderer, ",\"comm\":", 8);
			json_putquoted(&renderer, task.comm);
			json_write(&renderer, ",\"pcomm\":", 9);
			json_putquoted(&renderer, task.pcomm);
			json_putc(&renderer, '}');
			break;
		}
		case JSON_TOKEN_EMIT: {
			struct json_emit_record record;

			memcpy(&record, p, sizeof(record));
			p += sizeof(record);
			record.emit(emit_ctx, record.event, p, &renderer.out);
			p += record.snapshot_len;
			break;
		}
		case JSON_TOKEN_STRING:
			p = (const unsigned char *)json_puts(&renderer, (const char *)p) + 1;
			break;
		case JSON_TOKEN_KEY:
		case JSON_TOKEN_KEY_COMMA:
			p = (const unsigned char *)json_putkey(&renderer, (const char *)p, token == JSON_TOKEN_KEY_COMMA) + 1;
			break;
		case JSON_TOKEN_QUOTED:
			p = (const unsigned char *)json_putquoted(&renderer, (const char *)p) + 1;
			break;
		case JSON_TOKEN_RAW: {
			size_t n;

			memcpy(&n, p, sizeof(n));
			p += sizeof(n);
			json_write(&renderer, p, n);
			p += n;
			break;
		}
		case JSON_TOKEN_NULL:
			json_write(&renderer, "null", 4);
			break;
		case JSON_TOKEN_TRUE:
			json_write(&renderer, "true", 4);
			break;
		case JSON_TOKEN_FALSE:
			json_write(&renderer, "false", 5);
			break;
		case JSON_TOKEN_KEY_ONLY:
			break;
		default:
			json_putc(&renderer, token);
		}
	}
	*out = renderer.out;
}

static inline void json_work(void *ctx, void *ptr)
{
	struct json_job *job = ptr;

	(void)ctx;
	json_render(job->batch, job->batch_len, job->options, &job->out, job->emit_ctx);
}

static inline void json_direct_fallback(const char *reason)
{
	fprintf(stderr, "JSON direct I/O: buffered_fallback=1 reason=%s\n", reason);
}

static inline const char *json_btrfs_output(struct json_state *js, struct statfs *fs)
{
	struct stat st;
	int fd = js->writer.fd;

	if (js->out.f == stdout || fd < 0)
		return "output is not a named regular file";
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || !st.st_nlink)
		return "output is not a named regular file";
	if (st.st_size || ftello(js->out.f) != 0)
		return "output file must be empty";
	if (fstatfs(fd, fs) || fs->f_type != BTRFS_SUPER_MAGIC)
		return "output filesystem is not Btrfs";
	return NULL;
}

static inline void json_nocow_prepare(struct json_state *js)
{
	struct json_writer_state *writer = &js->writer;
	struct statfs fs;
	const char *reason = json_btrfs_output(js, &fs);
	int original_flags, flags, ret;
	bool changed = false;

	if (reason)
		goto fallback;
	if (ioctl(writer->fd, FS_IOC_GETFLAGS, &original_flags)) {
		reason = "cannot read Btrfs output file flags";
		goto fallback;
	}
	flags = (original_flags | FS_NOCOW_FL) & ~FS_COMPR_FL;
	if (flags != original_flags) {
		do {
			ret = ioctl(writer->fd, FS_IOC_SETFLAGS, &flags);
		} while (ret && errno == EINTR);
		if (ret) {
			reason = "cannot enable NOCOW on output file";
			goto fallback;
		}
		changed = true;
	}
	if (ioctl(writer->fd, FS_IOC_GETFLAGS, &flags) || !(flags & FS_NOCOW_FL) || (flags & FS_COMPR_FL)) {
		reason = "NOCOW output file flags were not applied";
		if (changed) {
			do {
				ret = ioctl(writer->fd, FS_IOC_SETFLAGS, &original_flags);
			} while (ret && errno == EINTR);
			if (ret) {
				writer->error = errno;
				fprintf(stderr, "Failed to restore JSON output file flags: %s\n", strerror(writer->error));
			}
		}
		goto fallback;
	}
	writer->nocow_active = true;
	fprintf(stderr, "JSON NOCOW: enabled nocow=1\n");
	return;

fallback:
	fprintf(stderr, "JSON NOCOW: enabled=0 reason=%s\n", reason);
}

static inline void json_direct_restore_fd(struct json_writer_state *writer)
{
	if (!writer->fd_flags_changed)
		return;
	int ret;

	do {
		ret = fcntl(writer->fd, F_SETFL, writer->original_fd_flags);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		if (!writer->error)
			writer->error = errno;
	} else {
		writer->fd_flags_changed = false;
	}
}

static inline void json_direct_rollback(struct json_writer_state *writer)
{
	json_direct_restore_fd(writer);
	free(writer->direct_data);
	writer->direct_data = NULL;
	writer->direct_active = false;
}

static inline void json_direct_start(struct json_state *js)
{
	struct json_writer_state *writer = &js->writer;
	const char *reason;
#ifdef O_DIRECT
	struct statfs fs;
	int flags, ret;

	reason = json_btrfs_output(js, &fs);
	if (reason)
		goto fallback;
	/* Btrfs reports its sector size here; statx can impose additional DIO alignment. */
	if (fs.f_bsize <= 0 || JSON_DIRECT_ALIGN % fs.f_bsize) {
		reason = "filesystem alignment exceeds 4096 bytes";
		goto fallback;
	}
#ifdef STATX_DIOALIGN
	struct statx sx;

	if (!statx(writer->fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &sx) && (sx.stx_mask & STATX_DIOALIGN)) {
		if (!sx.stx_dio_mem_align || !sx.stx_dio_offset_align ||
		    JSON_DIRECT_MEM_ALIGN % sx.stx_dio_mem_align || JSON_DIRECT_ALIGN % sx.stx_dio_mem_align ||
		    JSON_DIRECT_ALIGN % sx.stx_dio_offset_align) {
			reason = "kernel direct I/O alignment is incompatible";
			goto fallback;
		}
	}
#endif
	writer->original_fd_flags = fcntl(writer->fd, F_GETFL);
	if (writer->original_fd_flags < 0 || (writer->original_fd_flags & O_APPEND)) {
		reason = "output descriptor flags are incompatible";
		goto fallback;
	}
	if (fflush(js->out.f)) {
		reason = "cannot synchronize output stream";
		goto fallback;
	}
	ret = posix_memalign((void **)&writer->direct_data, JSON_DIRECT_MEM_ALIGN, JSON_DIRECT_CHUNK_SZ);
	if (ret) {
		reason = "cannot allocate aligned staging buffer";
		goto fallback;
	}
	if (ioctl(writer->fd, FS_IOC_GETFLAGS, &flags)) {
		reason = "cannot read Btrfs output file flags";
		goto fallback;
	}
	writer->nocow_active = !!(flags & FS_NOCOW_FL);
	do {
		ret = fcntl(writer->fd, F_SETFL, writer->original_fd_flags | O_DIRECT);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		reason = "cannot enable O_DIRECT on output descriptor";
		goto fallback;
	}
	writer->fd_flags_changed = true;
	writer->direct_active = true;
	return;
#else
	reason = "build does not expose O_DIRECT";
#endif
fallback:
	json_direct_rollback(writer);
	json_direct_fallback(reason);
}

static inline void json_direct_write(struct json_writer_state *writer, size_t len)
{
	const char *data = writer->direct_data;

	while (len) {
		ssize_t ret = pwrite(writer->fd, data, len, writer->direct_offset);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0) {
			writer->error = ret < 0 ? errno : EIO;
			return;
		}
		writer->direct_bytes += ret;
		writer->direct_writes++;
		writer->direct_offset += ret;
		if (ret % JSON_DIRECT_ALIGN) {
			writer->error = EIO;
			return;
		}
		data += ret;
		len -= ret;
	}
}

static inline void json_direct_pack(struct json_writer_state *writer, const char *data, size_t len)
{
	while (len && !writer->error) {
		size_t n = JSON_DIRECT_CHUNK_SZ - writer->direct_len;

		if (n > len)
			n = len;
		memcpy(writer->direct_data + writer->direct_len, data, n);
		writer->direct_len += n;
		data += n;
		len -= n;
		if (writer->direct_len == JSON_DIRECT_CHUNK_SZ) {
			json_direct_write(writer, writer->direct_len);
			writer->direct_len = 0;
		}
	}
}

static inline void json_job_free(struct json_job *job)
{
	free(job->batch);
	free(job->out.buffer.buf);
	free(job);
}

static inline void json_write_job(void *ctx, void *ptr)
{
	struct json_state *js = ctx;
	struct json_writer_state *writer = &js->writer;
	struct json_job *job = ptr;

	writer->float_zero_count += job->out.float_zero_count;
	writer->float_integer_count += job->out.float_integer_count;
	writer->float_fractional_count += job->out.float_fractional_count;
	writer->float_fallback_count += job->out.float_fallback_count;
	if (!writer->error) {
		writer->logical_size += job->out.buffer.len;
		if (writer->direct_active) {
			json_direct_pack(writer, job->out.buffer.buf, job->out.buffer.len);
		} else {
			fwrite(job->out.buffer.buf, 1, job->out.buffer.len, js->out.f);
		}
	}
	json_job_free(job);
}

static void *json_writer_main(void *ctx)
{
	struct json_state *js = ctx;
	struct json_writer_state *writer = &js->writer;

	prctl(PR_SET_NAME, "wprof_json_io", 0, 0, 0);
	pthread_mutex_lock(&writer->mutex);
	for (;;) {
		while (!writer->count && !writer->closing)
			pthread_cond_wait(&writer->work, &writer->mutex);
		if (!writer->count)
			break;
		struct json_job *job = writer->queue[writer->head];

		writer->queue[writer->head] = NULL;
		writer->head = (writer->head + 1) % 2;
		writer->count--;
		pthread_cond_signal(&writer->space);
		pthread_mutex_unlock(&writer->mutex);
		/* Keep draining after a failed write so producers cannot stay blocked. */
		json_write_job(js, job);
		pthread_mutex_lock(&writer->mutex);
	}
	pthread_mutex_unlock(&writer->mutex);
	if (writer->direct_active && !writer->error && writer->direct_len) {
		size_t len = (writer->direct_len + JSON_DIRECT_ALIGN - 1) / JSON_DIRECT_ALIGN * JSON_DIRECT_ALIGN;

		memset(writer->direct_data + writer->direct_len, 0, len - writer->direct_len);
		json_direct_write(writer, len);
	}
	return NULL;
}

static inline void json_writer_start(struct json_state *js)
{
	struct json_writer_state *writer = &js->writer;
	writer->fd = fileno(js->out.f);
	int err = pthread_mutex_init(&writer->mutex, NULL);

	if (err)
		goto fail;
	err = pthread_cond_init(&writer->work, NULL);
	if (err)
		goto destroy_mutex;
	err = pthread_cond_init(&writer->space, NULL);
	if (err)
		goto destroy_work;
	if (js->direct_io)
		json_direct_start(js);
	err = pthread_create(&writer->thread, NULL, json_writer_main, js);
	if (err) {
		if (writer->direct_active)
			json_direct_rollback(writer);
		goto destroy_space;
	}
	writer->started = true;
	if (writer->direct_active)
		fprintf(stderr, "JSON direct I/O: enabled alignment=%d chunk=%d nocow=%d\n",
			JSON_DIRECT_ALIGN, JSON_DIRECT_CHUNK_SZ, writer->nocow_active);
	return;

destroy_space:
	pthread_cond_destroy(&writer->space);
destroy_work:
	pthread_cond_destroy(&writer->work);
destroy_mutex:
	pthread_mutex_destroy(&writer->mutex);
fail:
	fprintf(stderr, "Failed to start JSON writer: %s; using synchronous writes\n", strerror(err));
	if (js->direct_io)
		json_direct_fallback("dedicated JSON writer could not start");
}

static inline void json_done(void *ctx, void *ptr)
{
	struct json_state *js = ctx;
	struct json_job *job = ptr;
	struct json_writer_state *writer = &js->writer;

	if (writer->started) {
		pthread_mutex_lock(&writer->mutex);
		while (writer->count == 2)
			pthread_cond_wait(&writer->space, &writer->mutex);
		writer->queue[(writer->head + writer->count) % 2] = job;
		writer->count++;
		pthread_cond_signal(&writer->work);
		pthread_mutex_unlock(&writer->mutex);
		return;
	}
	json_write_job(js, job);
}

static inline void json_submit(struct json_state *js)
{
	js->stack_frame_units = 0;
	if (!js->batch_len)
		return;
	struct json_job *job = calloc(1, sizeof(*job));

	job->batch = js->batch;
	job->batch_len = js->batch_len;
	job->options = js->out.options;
	job->emit_ctx = js->emit_ctx;
	js->batch = NULL;
	js->batch_len = 0;
	js->batch_capacity = 0;
	wpool_submit(js->pool, job);
}

static inline char *json_token_reserve(struct json_state *js, size_t n)
{
	if (js->batch_len && (js->batch_len >= js->batch_size || n > js->batch_size - js->batch_len))
		json_submit(js);
	if (n > js->batch_capacity) {
		size_t capacity = n > js->batch_size ? n : js->batch_size;

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
	if (js->pool) {
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

static inline char *json_field_reserve(struct json_state *js, enum json_key_id id, enum json_token kind, size_t n)
{
	char *p = json_token_reserve(js, 3 + n);

	*p = js->cnt[js->lvl] ? JSON_TOKEN_FIELD_COMMA : JSON_TOKEN_FIELD;
	p++;
	*p = id;
	p++;
	*p = kind;
	p++;
	js->cnt[js->lvl]++;
	return p;
}

static inline void json_field_data(struct json_state *js, enum json_key_id id, enum json_token kind,
				   const void *data, size_t n)
{
	memcpy(json_field_reserve(js, id, kind, n), data, n);
}

static inline void json_field_string(struct json_state *js, enum json_key_id id, const char *s)
{
	json_field_data(js, id, JSON_TOKEN_QUOTED, s, strlen(s) + 1);
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
	if (js->nocow || js->direct_io)
		js->writer.fd = fileno(js->out.f);
	if (js->nocow)
		json_nocow_prepare(js);
	if (!js->out.options.no_batching && !js->no_parallel)
		js->pool = wpool_new(js->max_workers, json_work, json_done, js);
	if (js->pool) {
		js->batch = malloc(js->batch_size);
		js->batch_capacity = js->batch_size;
		if (!js->no_writer)
			json_writer_start(js);
		else if (js->direct_io)
			json_direct_fallback("dedicated JSON writer is disabled");
	} else if (!js->out.options.no_batching) {
		js->out.buffer.buf = malloc(JSON_BUF_SZ);
		js->out.buffer.cap = JSON_BUF_SZ;
		js->out.buffer.len = 0;
	}
	if (!js->pool && js->direct_io)
		json_direct_fallback("parallel JSON writer is unavailable");
}

static inline int json_finish(struct json_state *js)
{
	struct json_writer_state *writer = &js->writer;

	if (js->pool) {
		json_submit(js);
		wpool_finish(js->pool);
		if (writer->started) {
			pthread_mutex_lock(&writer->mutex);
			writer->closing = true;
			pthread_cond_signal(&writer->work);
			pthread_mutex_unlock(&writer->mutex);
			pthread_join(writer->thread, NULL);
			if (writer->direct_active) {
				/* The aligned last write can extend EOF; expose only the actual JSON bytes. */
				if (!writer->error) {
					int ret;

					do {
						ret = ftruncate(writer->fd, writer->logical_size);
					} while (ret && errno == EINTR);
					if (ret)
						writer->error = errno;
				}
				json_direct_restore_fd(writer);
				if (fseeko(js->out.f, writer->logical_size, SEEK_SET) && !writer->error)
					writer->error = errno;
				free(writer->direct_data);
				writer->direct_data = NULL;
			}
		}
		free(js->batch);
		if (writer->started) {
			pthread_cond_destroy(&writer->space);
			pthread_cond_destroy(&writer->work);
			pthread_mutex_destroy(&writer->mutex);
			writer->started = false;
		}
	} else {
		json_flush(js);
	}
	sbuf_free(&js->out.buffer);
	json_float_stats_add(js, &js->writer);
	if (js->float_zero_count + js->float_integer_count + js->float_fractional_count + js->float_fallback_count)
		fprintf(stderr, "JSON float paths: zero=%llu integer=%llu fractional=%llu printf=%llu\n",
			(unsigned long long)js->float_zero_count,
			(unsigned long long)js->float_integer_count,
			(unsigned long long)js->float_fractional_count,
			(unsigned long long)js->float_fallback_count);
	for (size_t i = 0; i < js->task_cache_cnt; i++)
		sbuf_free(&js->task_cache[i]);
	free(js->task_cache);
	if (js->direct_io) {
		fprintf(stderr, "JSON direct I/O: %s direct_bytes=%llu direct_writes=%llu logical_bytes=%llu buffered_fallback=%d\n",
			writer->error ? "failed" : "completed", writer->direct_bytes, writer->direct_writes,
			(unsigned long long)writer->logical_size, !writer->direct_active);
	}
	return -writer->error;
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

static inline void json_vprintf_id(struct json_state *js, enum json_key_id id, const char *fmt, va_list ap)
{
	char buf[256], *value = buf;
	va_list ap2;

	va_copy(ap2, ap);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	if (n < 0) {
		if (id != JSON_KEY_COUNT)
			json_field_string(js, id, "");
		va_end(ap2);
		return;
	}
	if ((size_t)n >= sizeof(buf)) {
		value = malloc(n + 1);
		vsnprintf(value, n + 1, fmt, ap2);
	}
	if (id == JSON_KEY_COUNT)
		json_string(js, value, false);
	else
		json_field_string(js, id, value);
	if (value != buf)
		free(value);
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

static inline void json_vprintf(struct json_state *js, const char *fmt, va_list ap)
{
	json_vprintf_id(js, JSON_KEY_COUNT, fmt, ap);
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
		if (!js->pool && js->out.f && !js->out.options.no_batching && js->out.buffer.len >= JSON_BUF_SZ)
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
#define json_kv_uint_literal(js, key, value) \
	do { json_key_literal(js, key); json_value_uint(js, value); } while (0)
#define json_kv_null_literal(js, key) \
	do { json_key_literal(js, key); json_literal(js, "null", 4); } while (0)
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

static inline void json_subobj_start(struct json_state *js, const char *key)
{
	json_key(js, key);
	json_obj_start(js);
}

static inline void json_subarr_start(struct json_state *js, const char *key)
{
	json_key(js, key);
	json_arr_start(js);
}

static inline void json_key_id(struct json_state *js, enum json_key_id id)
{
	if (js->pool && !js->out.options.no_key_tokens)
		json_field_reserve(js, id, JSON_TOKEN_KEY_ONLY, 0);
	else
		json_key(js, json_known_keys[id].key);
}

static inline void json_subobj_start_id(struct json_state *js, enum json_key_id id)
{
	if (js->pool && !js->out.options.no_key_tokens) {
		json_key_id(js, id);
		json_obj_start(js);
	} else {
		json_subobj_start(js, json_known_keys[id].key);
	}
}

static inline void json_subarr_start_id(struct json_state *js, enum json_key_id id)
{
	if (js->pool && !js->out.options.no_key_tokens) {
		json_key_id(js, id);
		json_arr_start(js);
	} else {
		json_subarr_start(js, json_known_keys[id].key);
	}
}

static inline void json_kv_str_id(struct json_state *js, enum json_key_id id, const char *value)
{
	if (js->pool && !js->out.options.no_key_tokens)
		json_field_string(js, id, value);
	else
		json_kv_str(js, json_known_keys[id].key, value);
}

/* value_len excludes the NUL terminator. */
static inline void json_kv_str_id_n(struct json_state *js, enum json_key_id id, const char *value, size_t value_len)
{
	if (js->out.options.no_literal_lengths) {
		json_kv_str_id(js, id, value);
	} else if (js->pool) {
		if (js->out.options.no_key_tokens) {
			json_key(js, json_known_keys[id].key);
			json_token_data(js, JSON_TOKEN_QUOTED, value, value_len + 1);
		} else {
			json_field_data(js, id, JSON_TOKEN_QUOTED, value, value_len + 1);
		}
	} else {
		json_kv_str(js, json_known_keys[id].key, value);
	}
}

__attribute__((format(printf, 3, 4)))
static inline void json_kv_fmt_id(struct json_state *js, enum json_key_id id, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (js->pool && !js->out.options.no_key_tokens) {
		json_vprintf_id(js, id, fmt, ap);
	} else {
		json_key(js, json_known_keys[id].key);
		json_char(js, '"');
		json_vprintf(js, fmt, ap);
		json_char(js, '"');
	}
	va_end(ap);
}

static inline void json_kv_int_id(struct json_state *js, enum json_key_id id, long long value)
{
	if (js->pool && !js->out.options.no_key_tokens)
		json_field_data(js, id, JSON_TOKEN_INT, &value, sizeof(value));
	else
		json_kv_int(js, json_known_keys[id].key, value);
}

static inline void json_kv_uint_id(struct json_state *js, enum json_key_id id, unsigned long long value)
{
	if (js->pool && !js->out.options.no_key_tokens)
		json_field_data(js, id, JSON_TOKEN_UINT, &value, sizeof(value));
	else
		json_kv_uint(js, json_known_keys[id].key, value);
}

static inline void json_kv_null_id(struct json_state *js, enum json_key_id id)
{
	if (js->pool && !js->out.options.no_key_tokens)
		json_field_reserve(js, id, JSON_TOKEN_NULL, 0);
	else
		json_kv_null(js, json_known_keys[id].key);
}

static inline void json_kv_float_id(struct json_state *js, enum json_key_id id, const char *fmt, double value)
{
	if (js->pool && !js->out.options.no_key_tokens) {
		char *p = json_field_reserve(js, id, JSON_TOKEN_FLOAT, sizeof(fmt) + sizeof(value));

		memcpy(p, &fmt, sizeof(fmt));
		memcpy(p + sizeof(fmt), &value, sizeof(value));
	} else {
		json_kv_float(js, json_known_keys[id].key, fmt, value);
	}
}

static inline void json_kv_ts_id(struct json_state *js, enum json_key_id id, u64 ns)
{
	if (js->pool && !js->out.options.no_key_tokens)
		json_field_data(js, id, JSON_TOKEN_TS, &ns, sizeof(ns));
	else
		json_kv_ts(js, json_known_keys[id].key, ns);
}

static inline void json_kv_bool_id(struct json_state *js, enum json_key_id id, bool value)
{
	if (js->pool && !js->out.options.no_key_tokens)
		json_field_reserve(js, id, value ? JSON_TOKEN_TRUE : JSON_TOKEN_FALSE, 0);
	else
		json_kv_bool(js, json_known_keys[id].key, value);
}

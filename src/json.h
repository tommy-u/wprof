#include "wrust.h"

/*
 * LOW-LEVEL JSON BUILDER PRIMITIVES
 *
 * Builder helpers track nesting, scope, and comma state in json_state.
 * Formatting primitives write to json_buf.
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
	char *data;
	size_t len;
	size_t capacity;
	struct json_options options;
};

struct json_job {
	struct json_job *next;
	char *batch;
	size_t batch_len;
	size_t batch_capacity;
	struct json_options options;
	struct json_buf out;
};

struct json_state {
	struct json_buf out;
	struct wpool *pool;
	struct json_job *free_jobs;
	bool no_parallel;
	bool no_reuse;
	char *batch;
	size_t batch_len;
	size_t batch_capacity;
	int lvl;
	enum json_scope scope[5];
	int cnt[5]; /* comma tracking per level */
};

#define JSON_STATE_INIT(file) { .out = { .f = (file) }, .lvl = -1 }

#ifndef JSON_BUF_SZ
#define JSON_BUF_SZ (1024 * 1024)
#endif

static inline void json_flush(struct json_buf *b)
{
	if (b->len)
		fwrite(b->data, 1, b->len, b->f);
	b->len = 0;
}

static inline void json_write(struct json_buf *b, const void *p, size_t n)
{
	if (b->options.no_batching) {
		fwrite(p, 1, n, b->f);
		return;
	}
	if (n > b->capacity - b->len) {
		if (!b->f) {
			size_t capacity = b->capacity * 2;

			if (capacity < b->len + n)
				capacity = b->len + n;
			b->data = realloc(b->data, capacity);
			b->capacity = capacity;
		} else {
			json_flush(b);
			if (n > b->capacity) {
				fwrite(p, 1, n, b->f);
				return;
			}
		}
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

static inline void json_putc(struct json_buf *b, char c)
{
	if (b->options.no_batching) {
		fputc(c, b->f);
		return;
	}
	if (b->len == b->capacity)
		json_write(b, &c, 1);
	else
		b->data[b->len++] = c;
}

static inline char *json_decimal(char *end, unsigned long long value)
{
	do {
		*--end = '0' + value % 10;
		value /= 10;
	} while (value);
	return end;
}

static inline void json_int(struct json_buf *b, long long value)
{
	if (b->options.no_int_format) {
		if (b->options.no_batching) {
			fprintf(b->f, "%lld", value);
		} else {
			char buf[21];
			int n = snprintf(buf, sizeof(buf), "%lld", value);

			json_write(b, buf, n);
		}
		return;
	}
	char buf[20], *end = buf + sizeof(buf);
	unsigned long long magnitude = value;

	if (value < 0)
		magnitude = 0ULL - magnitude; /* unsigned subtraction also handles LLONG_MIN */
	char *p = json_decimal(end, magnitude);
	if (value < 0)
		*--p = '-';
	json_write(b, p, end - p);
}

static inline void json_uint(struct json_buf *b, unsigned long long value)
{
	if (b->options.no_int_format) {
		if (b->options.no_batching) {
			fprintf(b->f, "%llu", value);
		} else {
			char buf[21];
			int n = snprintf(buf, sizeof(buf), "%llu", value);

			json_write(b, buf, n);
		}
		return;
	}
	char buf[20], *end = buf + sizeof(buf);
	char *p = json_decimal(end, value);

	json_write(b, p, end - p);
}

/* Fixed-precision %f formats, including %.9lf of DBL_MAX, fit in this buffer. */
static inline void json_float(struct json_buf *b, const char *fmt, double value)
{
	if (b->options.no_batching) {
		fprintf(b->f, fmt, value);
		return;
	}
	char buf[400];
	int n = snprintf(buf, sizeof(buf), fmt, value);

	if (n < 0 || (size_t)n >= sizeof(buf))
		BUG("Invalid JSON float format\n");
	json_write(b, buf, n);
}

static inline void json_ts(struct json_buf *b, u64 ns)
{
	/* Below 2^22 seconds, double division error is less than half a nanosecond,
	 * so %.9lf rounds back to ns with the default rounding mode and C locale.
	 */
	if (b->options.no_int_ts || ns >= (1ULL << 22) * 1000000000ULL) {
		json_float(b, "%.9lf", ns / 1e9);
		return;
	}
	json_uint(b, ns / 1000000000ULL);
	char frac[10] = { '.' };
	u64 remainder = ns % 1000000000ULL;

	for (int i = 9; i > 0; i--) {
		frac[i] = '0' + remainder % 10;
		remainder /= 10;
	}
	json_write(b, frac, sizeof(frac));
}

static inline void json_escape(struct json_buf *b, unsigned char c)
{
	if (b->options.no_int_format) {
		if (b->options.no_batching) {
			fprintf(b->f, "\\u%04x", c);
		} else {
			char buf[7];
			int n = snprintf(buf, sizeof(buf), "\\u%04x", c);

			json_write(b, buf, n);
		}
		return;
	}
	const char hex[] = "0123456789abcdef";
	char buf[] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };

	json_write(b, buf, sizeof(buf));
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
static inline const char *json_puts(struct json_buf *b, const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	while (*p) {
		const unsigned char *run = p;

		while (*p >= 0x20 && *p < 0x80 && *p != '"' && *p != '\\')
			p++;
		if (p > run)
			json_write(b, run, p - run);

		unsigned char c = *p;
		switch (c) {
		case 0: return (const char *)p;
		case '"':  json_write(b, "\\\"", 2); p++; break;
		case '\\': json_write(b, "\\\\", 2); p++; break;
		case '\n': json_write(b, "\\n", 2); p++; break;
		case '\t': json_write(b, "\\t", 2); p++; break;
		case '\r': json_write(b, "\\r", 2); p++; break;
		case '\b': json_write(b, "\\b", 2); p++; break;
		case '\f': json_write(b, "\\f", 2); p++; break;
		default:
			if (c < 0x20) {
				json_escape(b, c);
				p++;
			} else {
				int n = json_utf8_seq_len(p);

				if (n) {
					json_write(b, p, n);
					p += n;
				} else {
					json_escape(b, c);
					p++;
				}
			}
		}
	}
	return (const char *)p;
}

static inline const char *json_putquoted(struct json_buf *b, const char *s)
{
	json_putc(b, '"');
	const char *end = json_puts(b, s);
	json_putc(b, '"');
	return end;
}

static inline const char *json_putkey(struct json_buf *b, const char *key, bool comma)
{
	if (comma)
		json_putc(b, ',');
	json_putc(b, '"');
	const char *end = json_puts(b, key);
	json_write(b, "\":", 2);
	return end;
}

/* Payloads are native-endian and unaligned; copied strings include their NUL terminator. */
static inline void json_render(const char *data, size_t len, struct json_options options, struct json_buf *out)
{
	const unsigned char *p = (const unsigned char *)data;
	const unsigned char *end = p + len;

	if (!out->data) {
		out->data = malloc(JSON_BUF_SZ);
		out->capacity = JSON_BUF_SZ;
	}
	out->len = 0;
	out->options = options;
	while (p < end) {
		unsigned char token = *p++;

		if (token == JSON_TOKEN_FIELD || token == JSON_TOKEN_FIELD_COMMA) {
			unsigned char id = *p++;

			if (token == JSON_TOKEN_FIELD_COMMA)
				json_putc(out, ',');
			json_write(out, json_known_keys[id].prefix, json_known_keys[id].prefix_len);
			token = *p++;
		}
		switch (token) {
		case JSON_TOKEN_INT: {
			long long value;

			memcpy(&value, p, sizeof(value));
			p += sizeof(value);
			json_int(out, value);
			break;
		}
		case JSON_TOKEN_UINT: {
			unsigned long long value;

			memcpy(&value, p, sizeof(value));
			p += sizeof(value);
			json_uint(out, value);
			break;
		}
		case JSON_TOKEN_FLOAT: {
			const char *fmt;
			double value;

			memcpy(&fmt, p, sizeof(fmt));
			p += sizeof(fmt);
			memcpy(&value, p, sizeof(value));
			p += sizeof(value);
			json_float(out, fmt, value);
			break;
		}
		case JSON_TOKEN_TS: {
			u64 ns;

			memcpy(&ns, p, sizeof(ns));
			p += sizeof(ns);
			json_ts(out, ns);
			break;
		}
		case JSON_TOKEN_TASK: {
			struct json_task_data task;

			memcpy(&task, p, sizeof(task));
			p += sizeof(task);
			json_putc(out, '{');
			if (task.tid) {
				json_write(out, "\"tid\":", 6);
				json_int(out, task.tid);
				json_putc(out, ',');
			}
			json_write(out, "\"pid\":", 6);
			json_int(out, task.pid);
			json_write(out, ",\"comm\":", 8);
			json_putquoted(out, task.comm);
			json_write(out, ",\"pcomm\":", 9);
			json_putquoted(out, task.pcomm);
			json_putc(out, '}');
			break;
		}
		case JSON_TOKEN_STRING:
			p = (const unsigned char *)json_puts(out, (const char *)p) + 1;
			break;
		case JSON_TOKEN_KEY:
		case JSON_TOKEN_KEY_COMMA:
			p = (const unsigned char *)json_putkey(out, (const char *)p, token == JSON_TOKEN_KEY_COMMA) + 1;
			break;
		case JSON_TOKEN_QUOTED:
			p = (const unsigned char *)json_putquoted(out, (const char *)p) + 1;
			break;
		case JSON_TOKEN_NULL:
			json_write(out, "null", 4);
			break;
		case JSON_TOKEN_TRUE:
			json_write(out, "true", 4);
			break;
		case JSON_TOKEN_FALSE:
			json_write(out, "false", 5);
			break;
		case JSON_TOKEN_KEY_ONLY:
			break;
		default:
			json_putc(out, token);
		}
	}
}

static inline void json_work(void *ctx, void *ptr)
{
	struct json_job *job = ptr;

	(void)ctx;
	json_render(job->batch, job->batch_len, job->options, &job->out);
}

static inline void json_job_release(struct json_state *js, struct json_job *job)
{
	if (js->no_reuse) {
		free(job->batch);
		free(job->out.data);
		free(job);
	} else {
		job->next = js->free_jobs;
		js->free_jobs = job;
	}
}

static inline void json_done(void *ctx, void *ptr)
{
	struct json_state *js = ctx;
	struct json_job *job = ptr;

	fwrite(job->out.data, 1, job->out.len, js->out.f);
	json_job_release(js, job);
}

static inline void json_submit(struct json_state *js)
{
	if (!js->batch_len)
		return;
	struct json_job *job = js->free_jobs;

	if (job)
		js->free_jobs = job->next;
	else
		job = calloc(1, sizeof(*job));
	job->next = NULL;
	job->batch = js->batch;
	job->batch_len = js->batch_len;
	job->batch_capacity = js->batch_capacity;
	job->options = js->out.options;
	js->batch = NULL;
	js->batch_len = 0;
	js->batch_capacity = 0;
	wpool_submit(js->pool, job);
	/* A full pool retires a written job during submit; reuse its batch for the producer. */
	job = js->free_jobs;
	if (!js->no_reuse && job) {
		js->batch = job->batch;
		js->batch_capacity = job->batch_capacity;
		job->batch = NULL;
		job->batch_len = 0;
		job->batch_capacity = 0;
	}
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

	*p++ = token;
	memcpy(p, data, n);
}

static inline void json_token_string(struct json_state *js, enum json_token token, const char *s)
{
	json_token_data(js, token, s, strlen(s) + 1);
}

static inline char *json_field_reserve(struct json_state *js, enum json_key_id id, enum json_token kind, size_t n)
{
	char *p = json_token_reserve(js, 3 + n);

	*p++ = js->cnt[js->lvl] ? JSON_TOKEN_FIELD_COMMA : JSON_TOKEN_FIELD;
	*p++ = id;
	*p++ = kind;
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
		json_write(&js->out, s, n);
}

static inline void json_char(struct json_state *js, char c)
{
	if (js->pool)
		*json_token_reserve(js, 1) = c;
	else
		json_putc(&js->out, c);
}

static inline void json_string(struct json_state *js, const char *s, bool quoted)
{
	if (js->pool)
		json_token_string(js, quoted ? JSON_TOKEN_QUOTED : JSON_TOKEN_STRING, s);
	else if (quoted)
		json_putquoted(&js->out, s);
	else
		json_puts(&js->out, s);
}

static inline void json_value_int(struct json_state *js, long long value)
{
	if (js->pool)
		json_token_data(js, JSON_TOKEN_INT, &value, sizeof(value));
	else
		json_int(&js->out, value);
}

static inline void json_value_uint(struct json_state *js, unsigned long long value)
{
	if (js->pool)
		json_token_data(js, JSON_TOKEN_UINT, &value, sizeof(value));
	else
		json_uint(&js->out, value);
}

static inline void json_value_float(struct json_state *js, const char *fmt, double value)
{
	if (js->pool) {
		char *p = json_token_reserve(js, 1 + sizeof(fmt) + sizeof(value));

		*p++ = JSON_TOKEN_FLOAT;
		memcpy(p, &fmt, sizeof(fmt));
		memcpy(p + sizeof(fmt), &value, sizeof(value));
	} else {
		json_float(&js->out, fmt, value);
	}
}

static inline void json_value_ts(struct json_state *js, u64 ns)
{
	if (js->pool)
		json_token_data(js, JSON_TOKEN_TS, &ns, sizeof(ns));
	else
		json_ts(&js->out, ns);
}

static inline void json_init(struct json_state *js)
{
	if (!js->out.options.no_batching && !js->no_parallel)
		js->pool = wpool_new(4, json_work, json_done, js);
	if (js->pool) {
		js->batch = malloc(JSON_BUF_SZ);
		js->batch_capacity = JSON_BUF_SZ;
	} else if (!js->out.options.no_batching) {
		js->out.data = malloc(JSON_BUF_SZ);
		js->out.capacity = JSON_BUF_SZ;
	}
}

static inline void json_finish(struct json_state *js)
{
	if (js->pool) {
		json_submit(js);
		wpool_finish(js->pool);
		free(js->batch);
		while (js->free_jobs) {
			struct json_job *job = js->free_jobs;

			js->free_jobs = job->next;
			free(job->batch);
			free(job->out.data);
			free(job);
		}
	} else {
		json_flush(&js->out);
		free(js->out.data);
	}
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
	if (js->lvl < 0) /* outermost level, emit newline */
		json_literal(js, "}\n", 2);
	else
		json_char(js, '}');
}

static inline void json_key(struct json_state *js, const char *key)
{
	if (js->pool)
		json_token_string(js, js->cnt[js->lvl] ? JSON_TOKEN_KEY_COMMA : JSON_TOKEN_KEY, key);
	else
		json_putkey(&js->out, key, js->cnt[js->lvl]);
	js->cnt[js->lvl]++;
}

static inline void json_subobj_start(struct json_state *js, const char *key)
{
	json_key(js, key);
	json_obj_start(js);
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

static inline void json_subarr_start(struct json_state *js, const char *key)
{
	json_key(js, key);
	json_arr_start(js);
}

static inline void json_arr_elem(struct json_state *js)
{
	if (js->cnt[js->lvl])
		json_char(js, ',');
	js->cnt[js->lvl]++;
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

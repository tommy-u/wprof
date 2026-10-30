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
	char *data;
	size_t len;
	size_t capacity;
	struct json_options options;
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
};

struct json_state {
	struct json_buf out;
	struct wpool *pool;
	struct json_writer_state writer;
	void *emit_ctx;
	bool no_parallel;
	bool no_writer;
	bool no_worker_emitters;
	bool direct_io;
	bool nocow;
	size_t max_workers;
	size_t batch_size;
	char *batch;
	size_t batch_len;
	size_t batch_capacity;
	int lvl;
	enum json_scope scope[5];
	int cnt[5]; /* comma tracking per level */
};

#define JSON_STATE_INIT(file) { .out = { .f = (file) }, .max_workers = 16, .batch_size = JSON_BUF_SZ, .lvl = -1 }

#ifndef JSON_BUF_SZ
#define JSON_BUF_SZ (1024 * 1024)
#endif

#define JSON_DIRECT_ALIGN 4096
#define JSON_DIRECT_MEM_ALIGN (2 * 1024 * 1024)
#define JSON_DIRECT_CHUNK_SZ (4 * 1024 * 1024)

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
static inline void json_render(const char *data, size_t len, struct json_options options, struct json_buf *out,
			       void *emit_ctx)
{
	const unsigned char *p = (const unsigned char *)data;
	const unsigned char *end = p + len;

	*out = (struct json_buf) {
		.data = malloc(JSON_BUF_SZ),
		.capacity = JSON_BUF_SZ,
		.options = options,
	};
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
		case JSON_TOKEN_EMIT: {
			struct json_emit_record record;

			memcpy(&record, p, sizeof(record));
			p += sizeof(record);
			record.emit(emit_ctx, record.event, p, out);
			p += record.snapshot_len;
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
	free(job->out.data);
	free(job);
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
		if (!writer->error) {
			writer->logical_size += job->out.len;
			if (writer->direct_active) {
				json_direct_pack(writer, job->out.data, job->out.len);
			} else {
				fwrite(job->out.data, 1, job->out.len, js->out.f);
			}
		}
		json_job_free(job);
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
	if (!writer->error)
		fwrite(job->out.data, 1, job->out.len, js->out.f);
	json_job_free(job);
}

static inline void json_submit(struct json_state *js)
{
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

	*p++ = token;
	memcpy(p, data, n);
}

/* Event and context pointers remain valid through finish; snapshots are copied into the batch. */
static inline char *json_emit_reserve(struct json_state *js, json_emit_fn emit, const void *event, size_t snapshot_len)
{
	struct json_emit_record record = { .emit = emit, .event = event, .snapshot_len = snapshot_len };
	char *p = json_token_reserve(js, 1 + sizeof(record) + snapshot_len);

	*p++ = JSON_TOKEN_EMIT;
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
		js->out.data = malloc(JSON_BUF_SZ);
		js->out.capacity = JSON_BUF_SZ;
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
		json_flush(&js->out);
		free(js->out.data);
	}
	if (js->direct_io) {
		fprintf(stderr, "JSON direct I/O: %s direct_bytes=%llu direct_writes=%llu logical_bytes=%llu buffered_fallback=%d\n",
			writer->error ? "failed" : "completed", writer->direct_bytes, writer->direct_writes,
			(unsigned long long)writer->logical_size, !writer->direct_active);
	}
	return -writer->error;
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

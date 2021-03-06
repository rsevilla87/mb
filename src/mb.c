#include <errno.h>		/* errno */
#include <getopt.h>		/* getopt_long() */
#include <inttypes.h>		/* PRIu64 */
#include <pthread.h>		/* pthread_create() */
#include <signal.h>		/* signal() */
#include <stdio.h>		/* stdout, stderr, fopen(), fclose() */
#include <stdlib.h>		/* free() */
#include <string.h>		/* strlen() */
#include <sys/stat.h>		/* stat() */
#include <sys/time.h>		/* gettimeofday() */
#include <unistd.h>		/* read(), close() */

#include "../version.h"
#include "../json/json.h"

#include "mb.h"
#include "merr.h"
#include "net.h"
#include "mcg.h"
#ifdef HAVE_SSL
#include "ssl.h"
#endif
#include "stats.h"		/* MIN/MAX() */

/* Module variables */
struct config cfg;				/* client options */
statistics stats;				/* statistics */
void (*requests_max_cb)() = NULL;

/* Global variables */
static connection *cs;
static int connections = 0;			/* number of connections defined in input requests file */
static volatile sig_atomic_t run;		/* thread termination variable */

struct http_parser_settings parser_settings = {
  .on_message_complete	= message_complete,
#if 0
  .on_header_field	= header_field,
  .on_header_value	= header_value,
  .on_message_begin	= message_begin,
  .on_headers_complete	= headers_complete,
#endif
};

static struct option longopts[] = {
  { "cookies",       no_argument,       NULL, 'c' },
  { "duration",      required_argument, NULL, 'd' },
  { "request-file",  required_argument, NULL, 'i' },
  { "response-file", required_argument, NULL, 'o' },
  { "quiet",         required_argument, NULL, 'q' },
  { "ramp-up",       required_argument, NULL, 'r' },
  { "ssl-version",   required_argument, NULL, 's' },
  { "threads",       required_argument, NULL, 't' },
  { "version",       no_argument,       NULL, 'v' },
  { NULL,            0,                 NULL,  0  }
};

/* Internal functions */
static void usage(int);
static inline char *mstrdup(const char *);
uint64_t time_us();
int stats_open(const char *);
int stats_init();
static char *format_bytes(char *dst, long double n);
static void stats_print();
int stats_close();
void exit_handler();
void mb_threads_auto();
void sig_int_term(int);
void signals_set();
static void json_check_value(const json_value *, json_type, const char *);
static void json_process_connection_tcp_keep_alive(const json_value *, connection *);
static void json_process_connection_tcp(const json_value *, connection *);
static void json_process_connection_headers(const json_value *, connection *);
static void json_process_connection_body(const json_value *, connection *);
static void json_process_connection_delay(const json_value *, connection *);
static void json_process_connection_close(const json_value *, connection *);
static int json_process_connection(const json_value *, connection *);
static int json_process_connections(const json_value *);
static void request_initialize_body_random(connection *, int);
int requests_read(const char *);
static int args_parse(struct config *, int, char **);
#ifdef HAVE_SSL
void ssl_ctx_init();
#endif
static int watchdog(aeEventLoop *, long long, void *);
void *thread_main(void *);
void threads_start();
static void requests_done();

static void usage(int ret) {
  fprintf(stderr, "Usage: " PGNAME " <options>\n"
                  "Options:\n"
                  "  -c, --cookies              use session cookies: %s\n"
                  "  -d, --duration <n>         test duration (including ramp-up) [s]: %"PRIu64"\n"
                  "  -i, --request-file <s>     input request file\n"
                  "  -o, --response-file <s>    output response stats file\n"
                  "  -q, --quiet                quiet mode\n"
                  "  -r, --ramp-up <n>          thread ramp-up time [s]: %"PRIu64"\n"
                  "  -s, --ssl-version <n>      SSL version: auto(0), SSLv3(1) - TLS1.2(4) [%d]\n"
                  "  -t, --threads <n>          number of worker threads: %"PRIu64"\n"
                  "  -v, --version              print version details\n"
                  "\n", cfg.cookies? "yes" : "no", cfg.duration, cfg.ramp_up, MB_TLS_VERSION, cfg.threads
          );

  if (ret >= 0) exit(ret);
}

static inline char *mstrdup(const char *s) {
  char *ret = NULL;

  if ((ret = strdup(s)) == NULL) {
    die(EXIT_FAILURE, "strdup(): unable to allocate %d bytes: %s (%d)\n", strlen(s), strerror(errno), errno);
  }

  return ret;
}

uint64_t time_us() {
  struct timeval t;
  gettimeofday(&t, NULL);
  return (t.tv_sec * 1000000) + t.tv_usec;
}

int stats_open(const char *file_out) {
  if (!file_out) return 1;

  if ((stats.fd = fopen(file_out, "w")) == NULL) {
    error("cannot open file `%s' for writing, using stdout\n", file_out);
    stats.fd = stdout;
    return 1;
  }

  return 0;
}

int stats_init() {
  stats.start = time_us();
  stats.fd = NULL;
  stats.err_conn = 0;
  stats.err_status = 0;

  /* open stats file for writing */
  return stats_open(cfg.file_resp);
}

static char *format_bytes(char *dst, long double n) {
  const char *suffix[] = { "B", "kiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB", NULL };
  const char **suffix_ptr = suffix;
  int base = 1024;

  while (n > base) {
    n /= base;
    if (*suffix_ptr) suffix_ptr++;
  }

  snprintf(dst, 11, "%0.2Lf%s", n, *suffix_ptr);	/* 11: 4 + 1 ('.') + 2 (decimal places) + 3 + 1 ('\0') */

  return dst;
}

/* Print statistics */
void stats_print() {
  char s1[12], s2[12];
  connection *cs_ptr = cs;
  uint64_t reqs = 0;
  uint64_t sent_bytes = 0;
  uint64_t recv_bytes = 0;
  uint64_t duration = time_us() - stats.start;
  long double rps, sent_mbps, recv_mbps;
  int n;

  for (n = 0; n < connections; n++, cs_ptr++) {
    reqs += cs_ptr->cstats.reqs_total;
    sent_bytes += cs_ptr->cstats.written_total;
    recv_bytes += cs_ptr->cstats.read_total;
  }

  rps = (long double)reqs*1000000/duration;
  recv_mbps = (long double)recv_bytes/duration*1000000;
  sent_mbps = (long double)sent_bytes/duration*1000000;
  fprintf(stdout, "Time: %0.2Lfs\n", (long double)duration/1000000);
  format_bytes(s1, sent_bytes); format_bytes(s2, sent_mbps);
  fprintf(stdout, "Sent: %s, %s/s\n", s1, s2);
  format_bytes(s1, recv_bytes); format_bytes(s2, recv_mbps);
  fprintf(stdout, "Recv: %s, %s/s\n", s1, s2);
  fprintf(stdout, "Hits: %"PRIu64", %0.2Lf/s\n", reqs, rps);
  if (stats.err_conn || stats.err_status || stats.err_parser)
    fprintf(stdout, "Errors connection: %"PRIu64", status: %"PRIu64", parser: %"PRIu64"\n", stats.err_conn, stats.err_status, stats.err_parser);
}

int stats_close() {
  if (stats.fd && stats.fd != stdout) fclose(stats.fd);

  return 0;
}

void exit_handler() {
  stats_print();
  stats_close();
  connections_free(cs);
#ifdef HAVE_SSL
  ssl_shutdown();
#endif
}

void mb_threads_auto() {
  int nproc = sysconf(_SC_NPROCESSORS_ONLN);

  if (nproc < 1) {
    nproc = MB_CFG_THREADS;	/* default number of worker threads */
  }

  cfg.threads = nproc;
}

void sig_int_term(int sig) {
  run = 0;
}

/* Set signal handlers */
void signals_set() {
  struct sigaction act;
  atexit(exit_handler);

  memset(&act, 0, sizeof(act));
  act.sa_handler = &sig_int_term;

  if (sigaction(SIGTERM, &act, NULL) < 0)
    error("sigaction(): %s (%d)\n", strerror(errno), errno);

  if (sigaction(SIGINT, &act, NULL) < 0)
    error("sigaction(): %s (%d)\n", strerror(errno), errno);
}

static void json_check_value(const json_value *value, json_type type, const char *err) {
  if (value == NULL)
    return;

  if (value->type != type)
    die(EXIT_FAILURE, "invalid input request file: %s\n", err);
}

static void json_process_connection_tcp_keep_alive(const json_value *value, connection *c) {
  int length, i;

  if (value == NULL)
    return;

  length = value->u.object.length;
  for (i = 0; i < length; i++) {
    const char *k = value->u.object.values[i].name;
    const json_value *v = value->u.object.values[i].value;
    if (!strcmp(k, "enable")) {
      json_check_value(v, json_boolean, "boolean expected for tcp.keep-alive.enable");
      c->tcp.keep_alive.enable = v->u.boolean;
    } else if (!strcmp(k, "idle")) {
      json_check_value(v, json_integer, "integer expected for tcp.keep-alive.idle");
      c->tcp.keep_alive.idle = v->u.integer;
    } else if (!strcmp(k, "intvl")) {
      json_check_value(v, json_integer, "integer expected for tcp.keep-alive.intvl");
      c->tcp.keep_alive.intvl = v->u.integer;
    } else if (!strcmp(k, "cnt")) {
      json_check_value(v, json_integer, "integer expected for tcp.keep-alive.cnt");
      c->tcp.keep_alive.cnt = v->u.integer;
    } else {
      die(EXIT_FAILURE, "invalid input request file, key tcp.keep-alive.%s\n", k);
    }
  }
}

static void json_process_connection_tcp(const json_value *value, connection *c) {
  int length, i;

  if (value == NULL)
    return;

  length = value->u.object.length;
  for (i = 0; i < length; i++) {
    const char *k = value->u.object.values[i].name;
    const json_value *v = value->u.object.values[i].value;
    if (!strcmp(k, "keep-alive")) {
      if (v->type == json_object)
        json_process_connection_tcp_keep_alive(v, c);
      else
        die(EXIT_FAILURE, "invalid input request file, tcp not an object\n");
    } else {
      die(EXIT_FAILURE, "invalid input request file, key tcp.%s\n", k);
    }
  }
}

static void json_process_connection_headers(const json_value *value, connection *c) {
  int length, i;

  if (value == NULL)
    return;

  length = value->u.object.length;
  if ((c->headers = calloc(length + 1, sizeof(key_value))) == NULL)
    die(EXIT_FAILURE, "calloc(): cannot allocate memory for headers\n");

  key_value *kv = c->headers;
  for (i = 0; i < length; i++, kv++) {
    const char *k = value->u.object.values[i].name;
    const json_value *v = value->u.object.values[i].value;

    kv->key = mstrdup(k);
    json_check_value(v, json_string, "string expected for headers");
    kv->value = mstrdup(v->u.string.ptr);
  }
}

static void json_process_connection_body(const json_value *value, connection *c) {
  int length, i;

  if (value == NULL)
    return;

  length = value->u.object.length;
  for (i = 0; i < length; i++) {
    const char *k = value->u.object.values[i].name;
    const json_value *v = value->u.object.values[i].value;
    if (!strcmp(k, "content")) {
      json_check_value(v, json_string, "string expected for body.content");
      if (c->req_body != NULL) free(c->req_body);
      c->req_body = mstrdup(v->u.string.ptr);
    } else if (!strcmp(k, "size")) {
      json_check_value(v, json_integer, "integer expected for body.size");
      c->req_body_size = v->u.integer;
    } else if (!strcmp(k, "type")) {
      json_check_value(v, json_string, "string expected for body.type");
      if (!strcmp(v->u.string.ptr, "random")) {
        c->req_body_type = body_random;
      } else if (!strcmp(v->u.string.ptr, "content")) {
        c->req_body_type = body_content;
      } else die(EXIT_FAILURE, "invalid body type: `%s'\n", v->u.string.ptr);
    } else {
      die(EXIT_FAILURE, "invalid input request file, key body.%s\n", k);
    }
  }

  if (c->req_body_type == body_random) {
    if (c->req_body) {
      warning("request body content provided but body random type specified; ignoring request's body.content\n");
      free(c->req_body); c->req_body = NULL;
    }

    if (c->req_body_size == 0) {
      die(EXIT_FAILURE, "request's body.size cannot be 0 when request's body random type is specified\n");
    }
  }
}

static void json_process_connection_delay(const json_value *value, connection *c) {
  int length, i;

  if (value == NULL)
    return;

  length = value->u.object.length;
  for (i = 0; i < length; i++) {
    const char *k = value->u.object.values[i].name;
    const json_value *v = value->u.object.values[i].value;
    if (!strcmp(k, "min")) {
      json_check_value(v, json_integer, "integer expected for delay.min");
      c->delay_min = v->u.integer;
    } else if (!strcmp(k, "max")) {
      json_check_value(v, json_integer, "integer expected for delay.max");
      c->delay_max = v->u.integer;
    } else {
      die(EXIT_FAILURE, "invalid input request file, key delay.%s\n", k);
    }
  }

  if (c->delay_min > c->delay_max) {
    die(EXIT_FAILURE, "invalid input request file, delay.min (%"PRIu64") > delay.max (%"PRIu64")\n", c->delay_min, c->delay_max);
  }
}

static void json_process_connection_close(const json_value *value, connection *c) {
  int length, i;

  if (value == NULL)
    return;

  length = value->u.object.length;
  for (i = 0; i < length; i++) {
    const char *k = value->u.object.values[i].name;
    const json_value *v = value->u.object.values[i].value;
    if (!strcmp(k, "client")) {
      json_check_value(v, json_boolean, "boolean expected for close.client");
      c->close_client = v->u.boolean;
    } else if (!strcmp(k, "linger")) {
      json_check_value(v, json_integer, "integer expected for close.linger");
      c->close_linger = true;
      c->close_linger_sec = v->u.integer;
    } else {
      die(EXIT_FAILURE, "invalid input request file, key close.%s\n", k);
    }
  }
}

static int json_process_connection(const json_value *value, connection *c) {
  int length, i, clients = 1;

  if (value == NULL)
    return -1;

  if(value->type != json_object)
    return -1;

  /* set the defaults */
  connection_init(c);

  length = value->u.object.length;
  for (i = 0; i < length; i++) {
    const char *k = value->u.object.values[i].name;
    const json_value *v = value->u.object.values[i].value;

    if (!strcmp(k, "tcp")) {
      /* TCP-related options */
      if (v->type == json_object)
        json_process_connection_tcp(v, c);
      else
        die(EXIT_FAILURE, "invalid input request file, tcp not an object\n");

      continue;
    }

    if (!strcmp(k, "delay")) {
      /* delay.min/max */
      if (v->type == json_object)
        json_process_connection_delay(v, c);
      else
        die(EXIT_FAILURE, "invalid input request file, delay not an object\n");

      continue;
    }

    if (!strcmp(k, "headers")) {
      /* headers */
      if (v->type == json_object)
        json_process_connection_headers(v, c);
      else
        die(EXIT_FAILURE, "invalid input request file, headers not an object\n");

      continue;
    }

    if (!strcmp(k, "body")) {
      /* body */
      if (v->type == json_object) {
        json_process_connection_body(v, c);
        if (c->req_body_type == body_random) request_initialize_body_random(c, i);
      } else if (v->type == json_string) {
        /* versions up to 0.1.5 used string for "body", provide some backward compatibility */
        if (c->req_body != NULL) free(c->req_body);
        c->req_body = mstrdup(v->u.string.ptr);
        warning("using string type for request body is deprecated, please change your input request file\n");
      } else die(EXIT_FAILURE, "invalid input request file, headers not an object\n");

      continue;
    }

    if (!strcmp(k, "close")) {
      /* close.client/linger */
      if (v->type == json_object)
        json_process_connection_close(v, c);
      else
        die(EXIT_FAILURE, "invalid input request file, close not an object\n");

      continue;
    }

    if (!strcmp(k, "host_from")) {
      json_check_value(v, json_string, "string expected for host_from");
      if (c->host_from != NULL) free(c->host_from);
      c->host_from = mstrdup(v->u.string.ptr);
    } else if (!strcmp(k, "host")) {
      json_check_value(v, json_string, "string expected for host");
      if (c->host != NULL) free(c->host);
      c->host = mstrdup(v->u.string.ptr);
    } else if (!strcmp(k, "port")) {
      json_check_value(v, json_integer, "integer expected for port");
      c->port = v->u.integer;
    } else if (!strcmp(k, "scheme")) {
      json_check_value(v, json_string, "string expected for scheme");
      if (!strcmp(v->u.string.ptr, "http")) c->scheme = http;
      else if (!strcmp(v->u.string.ptr, "https")) {
#ifndef HAVE_SSL
        die(EXIT_FAILURE, "ssl support not compiled in\n");
#endif
        c->scheme = https;
        cfg.ssl = true;
      }
      else die(EXIT_FAILURE, "invalid scheme %s\n", v);
    } else if (!strcmp(k, "method")) {
      json_check_value(v, json_string, "string expected for method");
      if (c->method != NULL) free(c->method);
      c->method = mstrdup(v->u.string.ptr);
    } else if (!strcmp(k, "path")) {
      json_check_value(v, json_string, "string expected for path");
      if (c->path != NULL) free(c->path);
      c->path = mstrdup(v->u.string.ptr);
    } else if (!strcmp(k, "max-requests")) {
      json_check_value(v, json_integer, "integer expected for max-requests");
      c->reqs_max = v->u.integer;
      if (c->reqs_max < 0) die(EXIT_FAILURE, "max-requests must be >= 0\n", optarg);
    } else if (!strcmp(k, "keep-alive-requests")) {
      json_check_value(v, json_integer, "integer expected for keep-alive-requests");
      c->keep_alive_reqs = v->u.integer;
      if (c->keep_alive_reqs < 0) die(EXIT_FAILURE, "keep-alive-requests must be >= 0\n", optarg);
    } else if (!strcmp(k, "tls-session-reuse")) {
      json_check_value(v, json_boolean, "boolean expected for tls-session-reuse");
      c->tls_session_reuse = v->u.boolean;
    } else if (!strcmp(k, "clients")) {
      json_check_value(v, json_integer, "integer expected for clients");
      clients = v->u.integer;
      if (clients > MB_MAX_CLIENTS)
        die(EXIT_FAILURE, "too many clients specified for a request (%d > %d)\n", clients, MB_MAX_CLIENTS);
    } else if (!strcmp(k, "ramp-up")) {
      json_check_value(v, json_integer, "integer expected for ramp-up time");
      c->ramp_up = v->u.integer;
    } else {
      die(EXIT_FAILURE, "invalid input request file, key %s\n", k);
    }
  }

  if (!c->host) {
    die(EXIT_FAILURE, "invalid input request file, host not defined\n");
  }

  if (!c->port) {
    die(EXIT_FAILURE, "invalid input request file, port not defined\n");
  }

  /* resolve the target host and service */
  if (!c->addr_to) {
    /* translating addresses comes at a cost, cache the structures */
    if (host_resolve(c->host, c->port, &c->addr_to) < 0)
      die(EXIT_FAILURE, "cannot resolve: %s:%d\n", c->host, c->port);
  }

  /* resolve the source host if any */
  if (c->host_from) {
    if (host_resolve(c->host_from, 0, &c->addr_from) < 0)
      die(EXIT_FAILURE, "cannot resolve: %s\n", c->host_from);
  }

  /* prepare HTTP data to send over a socket */
  http_requests_create(c);

  return clients;
}

static int json_process_connections(const json_value *value) {
  int i, j;
  int length, ret, connections;
  connection *c = cs;
  connection *cs_ptr;

  if (value == NULL)
    return 0;

  length = value->u.array.length;
  if (!length) die(EXIT_FAILURE, "no requests found in the input request file\n");
  connections = length;
  for (i = 0; i < length; i++) {
    if (value->u.array.values[i]->type != json_object)
      die(EXIT_FAILURE, "invalid input request file\n");

    ret = json_process_connection(value->u.array.values[i], c);
    if (ret < 0)
      die(EXIT_FAILURE, "invalid input request file (array %d)\n", i);

    if (ret > 1) {
      int offset = c - cs;
      connections += ret - 1;
      /* we have more than one client/connection per a given request */
      if ((cs = realloc(cs, (connections + 1) * sizeof(connection))) == NULL) {
        die(EXIT_FAILURE, "realloc() failed: %s (%d)\n", strerror(errno), errno);
      }
      /* copy the connection data to the uninitialised connections that follow */
      c = cs + offset;
      for (j = 1; j < ret; j++) {
        cs_ptr = c + j;
        memcpy(cs_ptr, c, sizeof(connection));
        if (cs_ptr->req_body_type == body_random) request_initialize_body_random(cs_ptr, j);
        cs_ptr->request = NULL;		/* shallow copy, prevent `free's when calling http_requests_create() */
        cs_ptr->request_cclose = NULL;	/* shallow copy, prevent `free's when calling http_requests_create() */
        http_requests_create(cs_ptr);	/* prepare HTTP data to send over a socket */
        cs_ptr->duplicate = true;	/* do not free any data structures on this connection */
      }
      c = cs + offset + ret;
      continue;
    }

    c += ret;
  }

  return connections;
}

static void request_initialize_body_random(connection *c, int i) {
  /* Initialize requests's body size with random data */
  size_t random_bytes_alloc = (c->req_body_size > MAX_REQ_LEN)? MAX_REQ_LEN: c->req_body_size;
  random_bytes_alloc += NUM2HEX_DIGITS(random_bytes_alloc) + 9;	/* account for TE chunked overhead: <len>\r\n + <body>\r\n + 0\r\n\r\n */
  if ((c->req_body_random = malloc(random_bytes_alloc)) == NULL)
    die(EXIT_FAILURE, "malloc(): cannot allocate memory for connection's random data\n");

  __uint128_t state = i * 2; /* different random data for every input "request" definition */
  mcg64_seed(&state);
  mcg64cpy(&state, c->req_body_random, random_bytes_alloc);
}

int requests_read(const char *file_in) {
  FILE *fp;
  struct stat filestatus;
  int file_size;
  char *file_contents;
  json_char *json;
  json_value *value;
  int connections = 0;

  if (stat(file_in, &filestatus) != 0) {
    die(EXIT_FAILURE, "file `%s' not found\n", file_in);
  }
  file_size = filestatus.st_size;
  file_contents = (char *) malloc(filestatus.st_size);
  if (file_contents == NULL) {
    die(EXIT_FAILURE, "malloc(): unable to allocate %d bytes\n", file_size);
  }

  fp = fopen(file_in, "rt");
  if (fp == NULL) {
    fclose(fp);
    free(file_contents);
    die(EXIT_FAILURE, "unable to open %s\n", file_in);
  }
  if (fread(file_contents, file_size, 1, fp) != 1) {
    fclose(fp);
    free(file_contents);
    die(EXIT_FAILURE, "fread(): unable to read content of %s\n", file_in);
  }
  fclose(fp);

  json = (json_char *) file_contents;

  value = json_parse(json, file_size);

  if (value == NULL) {
    free(file_contents);
    die(EXIT_FAILURE, "unable to parse json data\n");
  }

  if (value->type != json_array)
    die(EXIT_FAILURE, "invalid input request file\n");

  if ((cs = malloc((value->u.array.length + 1) * sizeof(connection))) == NULL)
    die(EXIT_FAILURE, "malloc(): cannot allocate memory for connections\n");

  connections = json_process_connections(value);
  cs[connections].t = NULL;	/* last (unused) connection (for looping over all connections) */

  json_value_free(value);
  free(file_contents);
  return connections;
}

static int args_parse(struct config *cfg, int argc, char **argv) {
  int c;
  char *p_err;

  cfg->cookies = MB_CFG_COOKIES;	/* default usage of cookies */
  cfg->duration = MB_CFG_DURATION;	/* default duration */
  cfg->file_req = NULL;
  cfg->file_resp = NULL;
  cfg->ramp_up = 0;
  cfg->ssl_version = MB_TLS_VERSION;
  cfg->ssl = false;

  while ((c = getopt_long(argc, argv, "cd:i:o:r:s:t:hqv", longopts, NULL)) != -1) {
    switch (c) {
    case 'c':
      cfg->cookies = true;
      break;

    case 'd':
      cfg->duration = strtol(optarg, &p_err, 0);
      if (p_err == optarg || *p_err) {
        die(EXIT_FAILURE, "duration: `%s' not an integer\n", optarg);
      }
      if (cfg->duration <= 0 || optarg[0] == '-') die(EXIT_FAILURE, "duration must be > 0\n", optarg);
      break;

    case 'i':
      cfg->file_req = optarg;
      break;

    case 'o':
      cfg->file_resp = optarg;
      break;

    case 'r':
      cfg->ramp_up = strtol(optarg, &p_err, 0);
      if (p_err == optarg || *p_err) {
        die(EXIT_FAILURE, "ramp-up: `%s' not an integer\n", optarg);
      }
      if (cfg->ramp_up < 0 || optarg[0] == '-') die(EXIT_FAILURE, "ramp-up must be > 0\n", optarg);
      break;

    case 's':
      cfg->ssl_version = strtol(optarg, &p_err, 0);
      if (p_err == optarg || *p_err) {
        die(EXIT_FAILURE, "ssl-version: `%s' not an integer\n", optarg);
      }
      if (cfg->ssl_version < 0 || cfg->ssl_version > 4 || optarg[0] == '-') die(EXIT_FAILURE, "ssl-version must be >= 0 and <= 4\n", optarg);
      break;

    case 't':
      cfg->threads = strtol(optarg, &p_err, 0);
      if (p_err == optarg || *p_err) {
        die(EXIT_FAILURE, "threads: `%s' not an integer\n", optarg);
      }
      if (cfg->threads <= 0 || optarg[0] == '-') die(EXIT_FAILURE, "number of threads must be > 0\n", optarg);
      break;

    case 'h':
      usage(EXIT_SUCCESS);
      break;

    case 'q':
      merr_suppress(s_info);		/* suppress info messages */
      break;

    case 'v':
      printf(PGNAME" %s [%s]\n", MB_VERSION, aeGetApiName());
      exit(EXIT_SUCCESS);
      break;

    case '?':
    case ':':
    default:
      usage(EXIT_FAILURE);
      break;
    }
  }

  if (cfg->cookies) {
    /* Parsing headers is expensive, turn it on only when needed. */
    parser_settings.on_header_field = header_field;
  }

  if (cfg->ramp_up >= cfg->duration) {
    error("ramp-up time (%"PRIu64") >= test duration (%"PRIu64")\n", cfg->ramp_up, cfg->duration);
    usage(EXIT_FAILURE);
  }

  if (cfg->file_req == NULL) {
    error("need to specify an input requests file\n");
    usage(EXIT_FAILURE);
  }

  return 0;
}

#ifdef HAVE_SSL
void ssl_ctx_init() {
  if (cfg.ssl && ssl_init(cfg.ssl_version) == NULL) {
    die(EXIT_FAILURE, "unable to initialize SSL\n");
  }
}
#endif

static int watchdog(aeEventLoop *loop, long long id, void *data) {
  if (run <= 0) aeStop(loop);

  return WATCHDOG_MS;
}

void *thread_main(void *arg) {
  thread *t = arg;
  long long time_event_id;

  uint64_t connections_per_thread = (connections > cfg.threads)? (connections / cfg.threads): 1;
  connection *cs_ptr;
  connection *cs_ptr_start = cs + (connections_per_thread * t->id);
  connection *cs_ptr_end = (t->id == cfg.threads - 1)? cs + connections: cs + (connections_per_thread * (t->id + 1));

  if (t->id >= connections) {
    warning("stopping thread %d, connections (%d) < threads (%d)\n", t->id + 1, connections, cfg.threads);
    goto out;
  }

  /* create main event loop */
  t->loop = aeCreateEventLoop(connections + cfg.threads + MB_FD_START);
  time_event_id = aeCreateTimeEvent(t->loop, WATCHDOG_MS, watchdog, NULL, NULL);
  if (time_event_id == AE_ERR) {
    die(EXIT_FAILURE, "cannot create time event: %s (%d)\n", strerror(errno), errno);
  }

  /* register socket connect callback */
  for (cs_ptr = cs_ptr_start; cs_ptr < cs_ptr_end; cs_ptr++) {
    cs_ptr->t = t;						/* point to the thread */
    cs_ptr->delayed = cs_ptr->delay_max;			/* connection will be delayed (delay_max always >= delay_min) */
    socket_connect(t->loop, 0, cs_ptr, 0);
  }

  /* start main loop */
  aeMain(t->loop);

  /* cleanup: delete time events */
  aeDeleteTimeEvent(t->loop, time_event_id);			/* remove watchdog */
  for (cs_ptr = cs_ptr_start; cs_ptr < cs_ptr_end; cs_ptr++) {
    aeDeleteTimeEvent(t->loop, cs_ptr->delayed_id);		/* remove any delayed requests */
  }

  /* stop loop */
  aeDeleteEventLoop(t->loop);

out:
  return (void *)t;
}

void threads_start() {
  pthread_attr_t attr;
  int i, r;
  void *thread_retval;			/* pointer to data returned by the terminating thread */
  uint64_t thread_delay, start;
  int64_t run_time;
  thread *threads;

  if (cfg.threads > connections) {
    info("threads (%d) > connections (%d): lowering the number of threads to %d\n", cfg.threads, connections, connections);
    cfg.threads = connections;
  }

  /* set callback function once the maximum number of requests is reached */
  requests_max_cb = requests_done;

  /* initialize and set thread detached attribute */
  if ((threads = calloc(cfg.threads, sizeof(thread))) == NULL)
    die(EXIT_FAILURE, "calloc(): cannot allocate memory for threads\n");
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  thread_delay = cfg.ramp_up? (cfg.ramp_up * 1000000) / cfg.threads: 0;
  start = time_us();

  /* start the worker threads */
  run = connections;			/* set this before starting threads */
  for (i = 0; i < cfg.threads; i++) {
    thread *t = &threads[i];
    t->id = i;
    r = pthread_create(&t->thread, &attr, thread_main, (void *)t);
    if (r) {
      die(EXIT_FAILURE, "unable to create thread %d: %s (%d)\n", i, strerror(errno), errno);
    }
    if (thread_delay && (i + 1) < cfg.threads) usleep(thread_delay);
  }

  /* free attribute and wait for the other threads */
  pthread_attr_destroy(&attr);

  /* wait for the threads to do their job */
  while (true) {
    run_time = (cfg.duration * 1000000) - (time_us() - start);
    if (run_time > 0 && run > 0) {
      usleep(MIN(run_time, WATCHDOG_MS * 1000));
    } else break;
  };
  run = 0;

  /* pthread_join */
  for (i = 0; i < cfg.threads; i++) {
    thread *t = &threads[i];
    r = pthread_join(t->thread, &thread_retval);
    if (r) {
      die(EXIT_FAILURE, "return value from pthread_join() was %d for thread %d\n", r, i);
    }
  }

  if (threads != NULL) free(threads);
}

/* called once we stop sending requests on a connection */
#if 1
pthread_mutex_t run_lock = PTHREAD_MUTEX_INITIALIZER;
static void requests_done() {
  pthread_mutex_lock(&run_lock);
  run--;
  pthread_mutex_unlock(&run_lock);
}
#else
static void requests_done() {
  __sync_fetch_and_sub(&run, 1);
}
#endif

int main(int argc, char **argv) {
  /* figure out the number of worker threads based on the hardware we have */
  mb_threads_auto();

  /* parse command-line arguments */
  args_parse(&cfg, argc, argv);

  /* override nameservers if environment variable(s) NAMESERVER<x> exist */
  override_ns();

  /* read the connections file */
  connections = requests_read(cfg.file_req);

  /* handle normal exit and catch some signals */
  signals_set();

#ifdef HAVE_SSL
  /* initialise ssl ctx */
  ssl_ctx_init();
#endif

  /* initialise the statistics structure and open stats file for writing if required to do so */
  stats_init();

  /* start up and shut down threads */
  threads_start();

  return 0;
}

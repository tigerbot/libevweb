#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <ev.h>
#include <evn.h>

#include "evweb.h"
#include "http-parser-callbacks.h"
#include "tcp-server.h"

#ifndef DEBUG_TCP_SERVER
  #ifdef DEBUG
    #define DEBUG_TCP_SERVER 1
  #else
    #define DEBUG_TCP_SERVER 0
  #endif
#endif

#if DEBUG_TCP_SERVER
  #define print_debug(...) printf("[tcp-server] " __VA_ARGS__)
#else
  #define print_debug(...)
#endif
#define print_status(...) printf("[tcp-server] " __VA_ARGS__)
#define print_err(...) fprintf(stderr, "[tcp-server] " __VA_ARGS__)

//static http_parser_settings parser_settings;
static evweb_server_settings* server_settings;
static struct evn_server* server;

static void on_server_error(EV_P, struct evn_server* server, struct evn_exception* error);
static void on_server_close(EV_P, struct evn_server* server);
static void on_connection(EV_P, struct evn_server* server, struct evn_stream* stream);

static void on_stream_data(EV_P, struct evn_stream* stream, void* data, int size);
static void on_stream_end(EV_P, struct evn_stream* stream);
static void on_stream_timeout(EV_P, struct evn_stream* stream);
static void on_stream_error(EV_P, struct evn_stream* stream, struct evn_exception* error);
static void on_stream_close(EV_P, struct evn_stream* stream, bool had_error);

void start_tcp_server(EV_P, int port, evweb_server_settings* settings) {
  server_settings = settings;
  server = evn_server_create(EV_A, on_connection);

  server->on_error = on_server_error;
  server->on_close = on_server_close;
  evn_server_listen(server, port, "0.0.0.0");
}

void close_tcp_server() {
  server->on_close = NULL;
  evn_server_close(server->EV_A, server);
}

static void on_server_error(EV_P, struct evn_server* server, struct evn_exception* error) {
  print_err("%s\n", error->message);
}

static void on_server_close(EV_P, struct evn_server* server) {
  print_err("server closed\n");
  exit(EXIT_FAILURE);
}

static void on_connection(EV_P, struct evn_server* server, struct evn_stream* stream) {
  evweb_http_processer* http_processer;

  print_debug("connection established\n");

  // initialize the http_processer object for this connection
  // we want to use calloc so all of the pointer start as NULL, and all the counters start at 0
  http_processer = calloc(1, sizeof (evweb_http_processer));
  stream->send_data = http_processer;
  http_processer->parser.data = stream;
  http_parser_init(&(http_processer->parser), HTTP_REQUEST);

  stream->on_data = on_stream_data;
  stream->on_end = on_stream_end;
  stream->on_timeout = on_stream_timeout;
  stream->on_error = on_stream_error;
  stream->on_close = on_stream_close;
  stream->oneshot = false;

  evn_stream_set_timeout(EV_A, stream, server_settings->max_keep_alive * 1000);
}

static void on_stream_data(EV_P, struct evn_stream* stream, void* data, int size) {
  int nparsed;
  http_parser_settings* parser_cbs;
  evweb_http_processer* parser = (evweb_http_processer*)stream->send_data;

  print_debug("received %d bytes of data over the connection (%p)\n", size, stream);

  if ( (parser->parser.data != stream) || ( ((struct evn_stream*)parser->parser.data)->EV_A != EV_A) )
  {
    print_err("parser's access to the stream or loop does not match ours\n");
    print_err("parser's stream = %p our stream = %p: parser's loop = %p our loop = %p\n", parser->parser.data, stream, ((struct evn_stream*)parser->parser.data)->EV_A, EV_A);
    parser->parser.data = stream;
    ((struct evn_stream*)parser->parser.data)->EV_A = EV_A;
  }

  parser_cbs = get_http_parser_settings();
  nparsed = http_parser_execute(&(parser->parser), parser_cbs, data, size);

  if (0 != parser->parser.upgrade)
  {
    print_err("upgrade requested. We don't support upgrades\n");
    evn_stream_write(EV_A, stream, "we don't support upgrades", strlen("we don't support upgrades"));
    evn_stream_destroy(EV_A, stream);
  }
  else if (nparsed != size)
  {
    print_err("parser did not read all of the data we feed it\n");
    evn_stream_destroy(EV_A, stream);
  }

  free(data);
}

static void on_stream_end(EV_P, struct evn_stream* stream) {
  print_debug("client (%p) sent FIN\n", stream);
}

static void on_stream_timeout(EV_P, struct evn_stream* stream) {
  if ( (stream->ready_state != evn_READ_ONLY) && (stream->ready_state != evn_CLOSED) )
  {
    print_debug("connection (%p) timed out at %f, sending FIN\n", stream, ev_now(EV_A));
    evn_stream_end(EV_A, stream);
    // only wait 10 secs after closing to destroy the connection.
    evn_stream_set_timeout(EV_A, stream, 10000);
    return;
  }
  print_err("connection (%p) timed out after we closed the connection, now destroying the connection\n", stream);
  evn_stream_destroy(EV_A, stream);
}

static void on_stream_error(EV_P, struct evn_stream* stream, struct evn_exception* error) {
  print_err("%s\n", error->message);
}

static void on_stream_close(EV_P, struct evn_stream* stream, bool had_error) {
  int i;
  evweb_header_line* current_line;
  evweb_http_processer* parser = (evweb_http_processer*)stream->send_data;

  // free everything in the http parser associated with this connection

  // first all of the request information
  current_line = parser->request.header_lines;
  for (i = 0; i < parser->request.num_header_lines; i += 1)
  {
    free(current_line->field);
    current_line->field = NULL;
    free(current_line->value);
    current_line->value = NULL;
    current_line += 1;
  }
  free(parser->request.header_lines);
  parser->request.header_lines = NULL;
  parser->request.num_header_lines = 0;
  parser->request.max_num_header_lines = 0;

  free(parser->request.url);
  parser->request.url = NULL;
  parser->request.url_length = 0;

  free(parser->request.body);
  parser->request.body = NULL;
  parser->request.body_length = 0;

  // then all the response information
  current_line = parser->response.header_lines;
  for (i = 0; i < parser->response.num_header_lines; i += 1)
  {
    free(current_line->field);
    current_line->field = NULL;
    free(current_line->value);
    current_line->value = NULL;
    current_line += 1;
  }
  free(parser->response.header_lines);
  parser->response.header_lines = NULL;
  parser->response.num_header_lines = 0;
  parser->response.max_num_header_lines = 0;

  free(parser->response.status_message);
  parser->response.status_message = NULL;

  free(parser->response.content);
  parser->response.content = NULL;
  parser->response.content_length = 0;

  free(parser->response.content_type);
  parser->response.content_type = NULL;

  free(parser);


  print_debug("connection (%p) closed at %f\n", stream, ev_now(EV_A));
  if (true == had_error)
  {
    print_err("error occured while closing connection (%p) (could have been from timeout)\n", stream);
  }
}


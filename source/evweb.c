#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <evn.h>

#include "evweb.h"
#include "tcp-server.h"

#ifndef DEBUG_EVWEB
  #ifdef DEBUG
    #define DEBUG_EVWEB 1
  #else
    #define DEBUG_EVWEB 0
  #endif
#endif

#if DEBUG_EVWEB
  #define print_debug(...) printf("[evweb] " __VA_ARGS__)
#else
  #define print_debug(...)
#endif
#define print_status(...) printf("[evweb] " __VA_ARGS__)
#define print_err(...) fprintf(stderr, "[evweb] " __VA_ARGS__)

static evweb_on_connection* request_handler;
static evweb_server_settings* server_settings;

static void close_connection_on_drain(EV_P, struct evn_stream* stream);

int interpret_header(evweb_http_processer* parser) {
  print_debug("header finished, reading in select values\n");
  parser->request.method = parser->parser.method;
  print_debug("header processing finished\n");
  return 0;
}

int finish_message(evweb_http_processer* parser) {
  print_debug("message finished, sending to handler\n");
  request_handler(&(parser->request), &(parser->response));
  print_debug("message handling finished\n");
  return 0;
}

int set_response_status(evweb_response* response, int status, char* message) {
  response->status = status;

  if ( (evn_CLOSED == (response->connection)->ready_state) || (evn_READ_ONLY == (response->connection)->ready_state) )
  {
    print_err("trying to set response status on a connection that has already ended (%s)\n", ((evweb_http_processer*)(response->connection->send_data))->request.url);
    return -1;
  }

  if ( (NULL != message) && (strlen(message) > 0) )
  {
    if (NULL != response->status_message)
    {
      free(response->status_message);
    }

    response->status_message = malloc(strlen(message) + 1);
    if (NULL == response->status_message)
    {
      print_err("failed to allocate memory for the header message: %s\n", strerror(errno));
      return errno;
    }
    print_debug("loading the header message into the response object\n");
    strncpy(response->status_message, message, strlen(message) + 1);
  }
  return 0;
}

int add_response_header(evweb_response* response, char* field, char* value) {
  int i;
  evweb_header_line* current_line;

  if ( (evn_CLOSED == (response->connection)->ready_state) || (evn_READ_ONLY == (response->connection)->ready_state) )
  {
    print_err("trying to add to response headers on a connection that has already ended (%s)\n", ((evweb_http_processer*)(response->connection->send_data))->request.url);
    return -1;
  }

  current_line = response->header_lines;
  for (i = 0; i < response->num_header_lines; i += 1)
  {
    if (0 == strcasecmp(current_line->field, field) )
    {
      free(current_line->field);
      free(current_line->value);
      break;
    }
    current_line += 1;
  }

  if (i == response->num_header_lines)
  {
    response->num_header_lines += 1;
    if (response->num_header_lines > response->max_num_header_lines)
    {
      response->max_num_header_lines *= 2;
      print_debug("expanding the number of response headers we can store to %d\n", response->max_num_header_lines);
      response->header_lines = realloc(response->header_lines, response->max_num_header_lines * sizeof (evweb_header_line));
      if (NULL == response->header_lines)
      {
        print_err("failed to allocate memory to expand the number of headers we can store: %s\n", strerror(errno));
        return errno;
      }
    }
    current_line = response->header_lines + response->num_header_lines - 1;
  }

  current_line->field_len = strlen(field);
  current_line->field = (char*)malloc(current_line->field_len + 1);
  if (NULL == current_line->field)
  {
    print_err("failed to allocate memory for response header field: %s\n", strerror(errno));

    current_line->field_len = 0;

    response->num_header_lines -= 1;
    return errno;
  }
  strncpy(current_line->field, field, current_line->field_len + 1);

  current_line->value_len = strlen(value);
  current_line->value = (char*)malloc(current_line->value_len + 1);
  if (NULL == current_line->value)
  {
    print_err("failed to allocate memory for response header field: %s\n", strerror(errno));

    current_line->value_len = 0;
    free(current_line->field);
    current_line->field_len = 0;

    response->num_header_lines -= 1;
    return errno;
  }
  strncpy(current_line->value, value, current_line->value_len + 1);

  return 0;
}

int clear_response_headers(evweb_response* response) {
  int i;
  evweb_header_line* current_line;

  if (evn_CLOSED == (response->connection)->ready_state)
  {
    print_err("trying to clear headers on a connection that has already ended (%s)\n", ((evweb_http_processer*)(response->connection->send_data))->request.url);
    return -1;
  }

  current_line = response->header_lines;
  for (i = 0; i < response->num_header_lines; i += 1)
  {
    free(current_line->field);
    current_line->field = NULL;
    free(current_line->value);
    current_line->value = NULL;
    current_line += 1;
  }
  response->num_header_lines = 0;

  return 0;
}

int set_response_body(evweb_response* response, void* body, size_t body_length, char* type) {

  if ( (evn_CLOSED == (response->connection)->ready_state) || (evn_READ_ONLY == (response->connection)->ready_state) )
  {
    print_err("trying to set response body on a connection that has already ended (%s)\n", ((evweb_http_processer*)(response->connection->send_data))->request.url);
    return -1;
  }

  clear_response_body(response);
  return add_to_response_body(response, body, body_length, type);
}

int add_to_response_body(evweb_response* response, void* body, size_t body_length, char* type) {
  size_t current_length;

  if ( (evn_CLOSED == (response->connection)->ready_state) || (evn_READ_ONLY == (response->connection)->ready_state) )
  {
    print_err("trying to add to response body on a connection that has already ended (%s)\n", ((evweb_http_processer*)(response->connection->send_data))->request.url);
    return -1;
  }

  if(body_length > 0)
  {
    print_debug("received %zu bytes of data to place in the body:\n", body_length);

    current_length = response->content_length;
    response->content_length += body_length;
    response->content = realloc(response->content, response->content_length);

    if (NULL == response->content)
    {
      print_err("failed to reallocate memory for response body: %s\n", strerror(errno));
      return errno;
    }
    print_debug("loading the body data into the response object (address %p)\n", response->content);
    memcpy(response->content + current_length, body, body_length);
  }

  if ( (NULL != type) && (strlen(type) > 0) )
  {
    if (NULL != response->content_type)
    {
      free(response->content_type);
    }

    response->content_type = malloc(strlen(type) + 1);
    if (NULL == response->content_type)
    {
      print_err("failed to allocate memory for application type: %s\n", strerror(errno));
      return errno;
    }
    print_debug("loading the application type (%s) into the response object\n", type);
    strncpy(response->content_type, type, strlen(type) + 1);
  }

  return 0;
}

int clear_response_body(evweb_response* response) {

  if (evn_CLOSED == (response->connection)->ready_state)
  {
    print_err("trying to clear body on a connection that has already ended (%s)\n", ((evweb_http_processer*)(response->connection->send_data))->request.url);
    return -1;
  }
  response->content_length = 0;

  if (NULL != response->content)
  {
    free(response->content);
    response->content = NULL;
  }

  if (NULL != response->content_type)
  {
    free(response->content_type);
    response->content_type = NULL;
  }

  return 0;
}

bool send_response(evweb_response* response) {
  struct evn_stream* stream = response->connection;
  char header[1024 + 128 * response->num_header_lines];
  size_t header_length = 0;
  char* message;

  int i;
  evweb_header_line* current_line;

  bool finished = false;

  if ( (evn_CLOSED == stream->ready_state) || (evn_READ_ONLY == stream->ready_state) )
  {
    print_err("trying to send response on a connection that has already ended (%s)\n", ((evweb_http_processer*)(response->connection->send_data))->request.url);
    return false;
  }

  if (-1 == response->status)
  {
    print_err("header status not sent, not sending anything over connection\n");
    return false;
  }

  if (NULL == response->status_message)
  {
    switch (response->status)
    {
      case 100:
        message = "Continue";
        break;
      case 101:
        message = "Switching Protocols";
        break;
      case 200:
        message = "OK";
        break;
      case 201:
        message = "Created";
        break;
      case 202:
        message = "Accepted";
        break;
      case 203:
        message = "Non-Authoritative Information";
        break;
      case 204:
        message = "No Content";
        break;
      case 205:
        message = "Reset Content";
        break;
      case 206:
        message = "Partial Content";
        break;
      case 400:
        message = "Bad Request";
        break;
      case 401:
        message = "Unauthorized";
        break;
      case 402:
        message = "Payment Required";
        break;
      case 403:
        message = "Forbidden";
        break;
      case 404:
        message = "Not Found";
        break;
      case 503:
        message = "Service Unavailable";
        break;

      default:
        message = "\0";
    }
  }
  else
  {
    message = response->status_message;
  }

  header_length += snprintf(header + header_length, (sizeof header) - header_length, "HTTP/1.1 %d %s\r\n", response->status, message);

  current_line = response->header_lines;
  for (i = 0; i < response->num_header_lines; i += 1)
  {
    header_length += snprintf(header + header_length, (sizeof header) - header_length, "%s: %s\r\n", current_line->field, current_line->value);
    current_line += 1;
  }

  if (response->content_length > 0)
  {
    header_length += snprintf(header + header_length, (sizeof header) - header_length, "Content-Length: %zu\r\n", response->content_length);
  }
  if (NULL != response->content_type)
  {
    header_length += snprintf(header + header_length, (sizeof header) - header_length, "Content-Type: %s\r\n", response->content_type);
  }

  header_length += snprintf(header + header_length, (sizeof header) - header_length, "\r\n");

  if ( (header_length >= sizeof header) || (header_length != strlen(header)) )
  {
    print_err("header length (%zu) and the string length of header (%zu) don't match\n", header_length, strlen(header));
    print_debug("%s", header);
  }

  finished = evn_stream_write(stream->EV_A, stream, header, strlen(header));

  if (evn_CLOSED == stream->ready_state)
  {
    print_err("connection closed while writing headers to stream\n");
    return false;
  }

  if (response->content_length > 0)
  {
    finished = evn_stream_write(stream->EV_A, stream, response->content, response->content_length);
    if (evn_CLOSED == stream->ready_state)
    {
      print_err("connection closed while writing body to stream\n");
      return false;
    }
  }

  clear_response_body(response);
  clear_response_headers(response);
  free(response->status_message);
  response->status_message = NULL;
  response->status = -1;

  return finished;
}

int end_response(evweb_response* response) {
  struct evn_stream* stream = response->connection;
  bool finished;

  if ( (evn_CLOSED == stream->ready_state) || (evn_READ_ONLY == stream->ready_state) )
  {
    print_err("trying to end response that has already ended (%s)\n", ((evweb_http_processer*)(response->connection->send_data))->request.url);
    return -1;
  }

  if (-1 == response->status)
  {
    finished = true;
  }
  else
  {
    add_response_header(response, "Connection", "close");
    finished = send_response(response);
  }

  if (evn_CLOSED == stream->ready_state)
  {
    print_err("connection closed while sending data (%s)\n", ((evweb_http_processer*)(response->connection->send_data))->request.url);
    return -1;
  }

  if (true == finished)
  {
    print_debug("closing connection\n");
    evn_stream_end(stream->EV_A, stream);
    return 0;
  }
  else
  {
    print_debug("did not send all data, will end connection on drain\n");
    stream->on_drain = close_connection_on_drain;
    return 1;
  }
}

void evweb_start_server(EV_P, int port, evweb_server_settings* settings, evweb_on_connection callback) {
  request_handler = callback;
  server_settings = settings;

  start_tcp_server(EV_A, port, settings);
}

void evweb_close_server() {
  close_tcp_server();
}

static void close_connection_on_drain(EV_P, struct evn_stream* stream) {
  print_debug("all data sent, we can now close connection\n");
  evn_stream_end(stream->EV_A, stream);
}

char* query_to_json(char* in_query, char* out_json, size_t json_size) {
  char* cur_pos;
  char* cur_write = NULL;
  char buff[3];
  int next_char;

  char key[256];
  bool has_value;
  char value[256];
  bool number;
  bool decimal;

  size_t written = 0;

  cur_pos = in_query;

  print_debug("in query: %s\n", in_query);

  written += snprintf(out_json + written, json_size - written, "{");
  while ('\0' != *cur_pos)
  {
    cur_write = key;
    has_value = true;
    while (cur_write < key + sizeof key)
    {
      *cur_write = *cur_pos;
      if ('=' == *cur_pos)
      {
        has_value = true;
        *cur_write = '\0';
        cur_pos += 1;
        break;
      }
      else if ( (';' == *cur_pos) || ('&' == *cur_pos) )
      {
        has_value = false;
        *cur_write = '\0';
        cur_pos += 1;
        break;
      }
      else if ('\0' == *cur_pos)
      {
        has_value = false;
        break;
      }
      cur_pos += 1;
      cur_write += 1;
    }
    key[(sizeof key) - 1] = '\0';
    if (false == has_value)
    {
      written += snprintf(out_json + written, json_size - written, "\"%s\": \"\", ", key);
      continue;
    }
    if ('\0' == *cur_pos)
    {
      break;
    }

    cur_write = value;
    number = true;
    decimal = false;
    while (cur_write < value + sizeof value)
    {
      switch (*cur_pos)
      {
        case '%':
          cur_pos += 1;
          buff[0] = *cur_pos;
          cur_pos += 1;
          buff[1] = *cur_pos;
          buff[2] = '\0';
          sscanf(buff, "%2x", &next_char);
          break;
        case '+':
          next_char = (int)' ';
          break;
        default:
          next_char = (int)*cur_pos;
      }
      switch (next_char)
      {
        case '\\':
          *cur_write = '\\';
          cur_write += 1;
          *cur_write = '\\';
          break;
        case '\'':
          *cur_write = '\\';
          cur_write += 1;
          *cur_write = '\'';
          break;
        case '\n':
          *cur_write = '\\';
          cur_write += 1;
          *cur_write = 'n';
          break;
        default:
          *cur_write = (char)next_char;
      }
      if ( (';' == *cur_pos) || ('&' == *cur_pos) )
      {
        *cur_write = '\0';
        cur_pos += 1;
        break;
      }
      else if ('\0' == *cur_pos)
      {
        break;
      }
      // if we haven't already determined it's not a number, and the character isn't a digit, then it's probably not a number
      if ( (true == number) && ((*cur_write < '0') || (*cur_write > '9')) )
      {
        number = false;
        // each number is allowed one '-' at the beginning and one '.' anywhere
        if (('.' == *cur_write) && (false == decimal))
        {
          decimal = true;
          number = true;
        }
        if (('-' == *cur_write) && (value == cur_write))
        {
          number = true;
        }
      }
      cur_write += 1;
      cur_pos += 1;
    }
    value[(sizeof value) - 1] = '\0';
    if ( (false == number) && (0 != strcmp(value, "true")) && (0 != strcmp(value, "false")) )
    {
      written += snprintf(out_json + written, json_size - written, "\"%s\": \"%s\", ", key, value);
    }
    else
    {
      written += snprintf(out_json + written, json_size - written, "\"%s\": %s, ", key, value);
    }
  }
  // we want to remove the trailing ", "
  if (written > 2)
  {
    written -= 2;
  }
  written += snprintf(out_json + written, json_size - written, "}");

  print_debug("out json: %s\n", out_json);
  return out_json;
}


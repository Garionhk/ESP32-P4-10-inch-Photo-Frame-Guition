// ===========================================================================
// fetch.h -- HTTP/HTTPS requests
// ===========================================================================
// The CYD project learned this the hard way and the lesson carries over:
// parse from a BUFFER, never straight from the stream. Parsing a chunked
// response as it arrives mis-reads it and yields an all-zeros document with
// no error anywhere.
#pragma once

#include <stddef.h>
#include <stdbool.h>

// TLS trust for a request. Immich is commonly self-hosted, so all three of
// these are real deployments rather than hypotheticals.
typedef enum {
    FETCH_TLS_NONE = 0,   // plain http://
    FETCH_TLS_BUNDLE,     // https:// with a publicly trusted certificate
    FETCH_TLS_PEM,        // https:// with a pasted CA (self-signed / internal)
} fetch_tls_t;

typedef struct {
    const char *url;
    const char *method;        // "GET" or "POST"; NULL means GET
    const char *body;          // request body, or NULL
    const char *content_type;  // e.g. "application/json"
    const char *api_key;       // sent as x-api-key when non-NULL
    fetch_tls_t tls;
    const char *ca_pem;        // used when tls == FETCH_TLS_PEM
    size_t      limit;         // maximum bytes to read back
} fetch_req_t;

// Returns a NUL-terminated body the caller must free(), or NULL.
char *fetch_json(const char *url, size_t limit);          // convenience GET
char *fetch_request(const fetch_req_t *req, int *out_len);

// Stream a URL straight to a file on the card. Nothing is buffered whole, so
// a large photo is bounded by the card rather than by RAM.
bool fetch_to_file(const fetch_req_t *req, const char *path, size_t *out_bytes);

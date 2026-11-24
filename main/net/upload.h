// ===========================================================================
// upload.h -- streaming multipart/form-data receiver
// ===========================================================================
// Writes straight to the card as bytes arrive, so the limit is the card's
// free space rather than free RAM. Buffering a whole upload first would cap
// photos at a few hundred kilobytes on a device whose internal heap is
// already the scarcest thing it has.
#pragma once

#include "esp_http_server.h"

esp_err_t upload_handle(httpd_req_t *req);

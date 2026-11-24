#!/usr/bin/env bash
# Source this before any idf.py command:   source tools/env.sh
#
# Beyond the usual export.sh, this pins SSL_CERT_FILE to certifi's bundle.
# The python.org 3.12 framework on this machine ships without the OpenSSL cert
# symlink, so the IDF tool installer and the component manager both fail with
# CERTIFICATE_VERIFY_FAILED without it. Setting it here keeps the fix scoped to
# the shell rather than modifying the system Python install.
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"

_certifi=$(/Library/Frameworks/Python.framework/Versions/3.12/bin/python3 \
             -c "import certifi;print(certifi.where())" 2>/dev/null)
if [ -n "$_certifi" ]; then
    export SSL_CERT_FILE="$_certifi"
    export REQUESTS_CA_BUNDLE="$_certifi"
fi
unset _certifi

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"

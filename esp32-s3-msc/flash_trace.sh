#!/usr/bin/env bash
# Car capture build: the normal car build (flash.sh) plus MSC_TRACE.
exec "$(dirname "$0")/flash.sh" --trace "$@"

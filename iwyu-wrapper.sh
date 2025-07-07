#!/bin/bash
# Drop unwanted flags, e.g. -ftime-trace

NEW_ARGS=()
for arg in "$@"; do
  if [[ "$arg" == "-ftime-trace" ]] || [[ "$arg" == -fdebug-prefix-map=* ]] || [[ "$arg" == -fmacro-prefix-map=* ]]; then
    continue
  fi
  NEW_ARGS+=("$arg")
done

exec /usr/bin/include-what-you-use "${NEW_ARGS[@]}"

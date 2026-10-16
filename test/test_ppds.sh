#!/bin/bash
set -eu

script_dir=$(cd "$(dirname "$0")/.." && pwd)
exec "$script_dir/p1600dn_manager.command" --test-ppds "$@"

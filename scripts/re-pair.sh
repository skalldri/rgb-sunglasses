#!/usr/bin/env bash
# Thin wrapper around scripts/re-pair.py — kept as the entry point so the /re-pair
# skill, the docs, and the hw-lock-guard pattern that all reference `re-pair.sh`
# continue to work. The real implementation (and its board+app lock check) lives in
# re-pair.py; it was moved to Python because the SAFE, device-name-verified forget
# (never unpair the wrong device on a phone with many bonds) outgrew bash.
exec python3 "$(cd "$(dirname "$0")" && pwd)/re-pair.py" "$@"

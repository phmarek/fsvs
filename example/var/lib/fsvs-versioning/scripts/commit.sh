#!/bin/sh

fsvs ci /etc -m "${1:-Auto-commit after dpkg}" -q


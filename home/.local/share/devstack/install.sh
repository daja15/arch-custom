#!/bin/sh
# Встановлює polkit-правило для devstack (запускати через sudo)
set -e
install -m 644 -o root -g root "$(dirname "$0")/50-devstack-docker.rules" /etc/polkit-1/rules.d/50-devstack-docker.rules
echo "OK: /etc/polkit-1/rules.d/50-devstack-docker.rules"

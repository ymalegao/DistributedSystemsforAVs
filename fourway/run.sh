#!/bin/bash
# Run the default ResDB fourway simulation with Qtenv.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec "${SCRIPT_DIR}/run-resdb-simulation.sh" -u Qtenv -c FourVehiclesResDB "$@"

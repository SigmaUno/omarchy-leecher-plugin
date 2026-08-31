#!/bin/sh
# Run the whole test suite. Non-zero exit if anything fails.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
rc=0

echo "########## unit ##########"
sh "$here/unit/run.sh" || rc=1

echo
echo "###### integration ######"
sh "$here/integration/test_controls.sh" || rc=1

echo
if [ "$rc" -eq 0 ]; then
    echo "ALL SUITES PASSED"
else
    echo "SUITE FAILURES"
fi
exit $rc

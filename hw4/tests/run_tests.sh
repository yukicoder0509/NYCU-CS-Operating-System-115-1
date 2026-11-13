#!/usr/bin/env bash
# Usage: ./run_tests.sh   (from hw4/tests)
cd "$(dirname "$0")/.."
make -s main multilevelBF.so || exit 1
pass=0; fail=0
for t in tests/*.txt; do
    exp="Max Free Chunk Size = $(cat "${t%.txt}.expected")"
    cp "$t" test.txt
    got=$(LD_PRELOAD=$PWD/multilevelBF.so ./main 2>&1)
    if [ "$got" == "$exp" ]; then echo "PASS $t"; ((pass++))
    else echo "FAIL $t"; echo "   expected: $exp"; echo "   got:      $got"; ((fail++)); fi
done
rm -f test.txt
echo "$pass passed, $fail failed"

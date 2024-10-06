#!/bin/bash

sox ../../../micsamples/desmume_1.wav --bits 8 -c 1 --encoding unsigned-integer -r 11030 tmp.raw
hexdump tmp.raw -Xv > tmp

N_SAMPLES=$(stat -c%s tmp)

echo '// Generated with convert-sample.sh'
echo
echo '#include "types.h"'
echo
echo 'const u8 customMicSample[] = {'
cat tmp | sed -E 's/^[0-9a-f]{3,}\s*/  /g; s/([0-9a-f]{2})\s?/0x\1,/g'
echo '};'

rm -f tmp.raw
rm -f tmp
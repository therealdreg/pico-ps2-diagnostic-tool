#!/bin/bash

echo "Checking... please wait and be patient!..."

EXPECTED_SIZE=983077

if [ $# -ne 1 ]; then
    echo "Usage: $0 <file>"
    exit 1
fi

FILE="$1"

if [ ! -f "$FILE" ]; then
    echo "Error: File '$FILE' not found."
    exit 1
fi

LINE_NUMBER=0
VALID=true

while IFS= read -r line || [ -n "$line" ]; do
    ((LINE_NUMBER++))
    echo "Checking line $LINE_NUMBER..."   

    if [[ "$line" == unsigned\ char\ array_* ]]; then
        LINE_SIZE=$(echo "$line" | dd bs=1 count=10000000 2>/dev/null | wc -c)
        
        if [ "$LINE_SIZE" -ne "$EXPECTED_SIZE" ]; then
            echo "Error on line $LINE_NUMBER: size $LINE_SIZE bytes, expected $EXPECTED_SIZE bytes"
            echo "Line $LINE_NUMBER content (first 50 chars): ${line:0:50}"
            VALID=false
        fi
    fi
done < "$FILE"

if $VALID; then
    echo "OK: All lines have the correct size."
else
    echo "ERROR: Some lines have an incorrect size."
fi

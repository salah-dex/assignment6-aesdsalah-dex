#!/bin/sh
# Author: Salaheddine GHANEM

if [ $# -ne 2 ]
then
    echo "Wrong exptected args"
    exit 1
fi

FILE_DIR=$1
SEARCH_STR=$2

# Check if the provided path is a directory
if [ ! -d "$FILE_DIR" ]; then
    echo "Error: $FILE_DIR is not a directory."
    exit 1
fi
# Count the number of files and matching lines
X=$(find "$FILE_DIR" -type f | wc -l)
Y=$(grep -r "$SEARCH_STR" "$FILE_DIR" | wc -l)

# Print the result
echo "The number of files are $X and the number of matching lines are $Y."
#!/bin/sh

# Check if the correct number of arguments are provided
if [ "$#" -ne 2 ]; then
    echo "Error: Two arguments required - <full_path_to_file> <text_string>"
    exit 1
fi

WRITE_FILE=$1
WRITE_STR=$2

# Create the directory path if it doesn't exist
mkdir -p "$(dirname "$WRITE_FILE")"

if echo "$WRITE_STR" > "$WRITE_FILE"; then
    echo "File created successfully."
else
    echo "Error: Could not create the file."
    exit 1
fi
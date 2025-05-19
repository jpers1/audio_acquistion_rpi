#!/usr/bin/env bash

# Check if an argument was provided
if [ -z "$1" ]; then
  echo "Usage: $0 <suffix_string>"
  exit 1
fi

# Construct the output filename by concatenating prefix and suffix
FILENAME="~/audiofiles-klipanje$1.tar.gz"

# Create the tar.gz archive
tar -cvzf "$FILENAME" /dev/shm/audiofiles/

echo "Archive created: $FILENAME"

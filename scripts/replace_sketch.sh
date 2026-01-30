#!/bin/bash

# Usage: ./replace_sketch.sh <ino_file_path> <output_folder>
# - <ino_file_path>: Relative or absolute path to your .ino file (e.g., sketch.ino or ../src/sketch.ino)
# - <output_folder>: Path to the folder where the modified main.cpp should be written (will be created if it doesn't exist)

if [ $# -ne 2 ]; then
    echo "Usage: $0 <ino_file_path> <output_folder>"
    exit 1
fi

INO_FILE="$1"
OUTPUT_FOLDER="$2"

# Create output folder if it doesn't exist
mkdir -p "$OUTPUT_FOLDER"

# Assume main.cpp template is in the current directory; adjust this path if needed
TEMPLATE="replace_sketch.cpp"

if [ ! -f "$TEMPLATE" ]; then
    echo "Error: main.cpp template not found in current directory."
    exit 1
fi

# Compute relative path from output folder to .ino file
# This ensures #include works relative to where the new main.cpp will be compiled from
RELATIVE_INO=$(realpath --relative-to="$OUTPUT_FOLDER" "$INO_FILE")

# Replace ${SKETCH} with the relative path (escaped for sed, and wrapped in quotes)
sed "s/\${SKETCH}/\"${RELATIVE_INO}\"/g" "$TEMPLATE" > "$OUTPUT_FOLDER/main.cpp"

echo "Modified main.cpp written to $OUTPUT_FOLDER/main.cpp with #include \"$RELATIVE_INO\""
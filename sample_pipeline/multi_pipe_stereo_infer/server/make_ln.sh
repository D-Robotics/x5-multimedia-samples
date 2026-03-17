#!/bin/bash
TARGET_DIR="./hobot_stereonet/DStereo_X5/3rdparty/lib_opencv4.5.4/lib"

for file in "$TARGET_DIR"/*.4.5; do
    if [[ -f "$file" ]]; then
        base_name="${file%.4.5}"

        target_file="${base_name}.4.5.4"

        if [[ -f "$target_file" ]]; then
            rm "$file"
            ln -s "$(basename "$target_file")" "$file"
            echo "make ln: $file -> $(basename "$target_file")"

            rm "$base_name"
            ln -s "$(basename "$target_file")" "$base_name"

        else
            echo "skip: $target_file"
        fi
    fi
done


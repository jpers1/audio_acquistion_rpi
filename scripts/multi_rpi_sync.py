#!/usr/bin/env python3

import argparse
import os
import soundfile as sf
import numpy as np

def read_utc_timestamp(file_name):
    """
    Reads a single-line UTC timestamp from file_name, returns it as float.
    """
    with open(file_name, 'r') as f:
        line = f.readline().strip()
        if line:
            return float(line)
        raise ValueError(f"Error: {file_name} is empty or invalid.")

def read_first_relative_timestamp(file_name):
    """
    Reads the first line of a timestamps.txt file, interpreting it as the
    first sample's relative time (seconds) to the start of the stream.
    """
    with open(file_name, 'r') as f:
        line = f.readline().strip()
        if line:
            return float(line)
        raise ValueError(f"Error: {file_name} is empty or invalid.")

def adjust_audio(input_wav, output_wav, offset_seconds):
    """
    Trims 'offset_seconds' from the start of input_wav and writes result to output_wav.
    """
    # Load the audio using soundfile
    audio_data, sample_rate = sf.read(input_wav, dtype='float32')
    num_frames = audio_data.shape[0]
    if audio_data.ndim == 1:
        num_channels = 1
    else:
        num_channels = audio_data.shape[1]

    # Calculate the number of frames to trim
    offset_frames = int(round(offset_seconds * sample_rate))

    print(f"Trimming {offset_seconds:.6f} s (≈ {offset_frames} frames) from {input_wav}")

    # If offset is greater than the total length, result is empty
    if offset_frames < num_frames:
        adjusted_data = audio_data[offset_frames:]
    else:
        adjusted_data = np.array([], dtype=np.float32).reshape(0, num_channels if num_channels > 1 else ())

    # Write the adjusted audio to a new file
    sf.write(output_wav, adjusted_data, sample_rate)
    print(f" -> Wrote synced file: {output_wav}")

def calculate_offsets(utc_files, timestamp_files):
    """
    1. For each device, read UTC start time + the first relative sample time.
    2. Compute 'absolute start time'.
    3. Determine the latest of these absolute start times as a reference.
    4. offset[i] = (latest absolute start) - (this device's absolute start).
    Returns a dict { index_in_list : offset_in_seconds }.
    """
    if len(utc_files) != len(timestamp_files):
        raise ValueError("Mismatch in number of UTC files vs. timestamp files.")

    # Read each file
    utc_starts = [read_utc_timestamp(f) for f in utc_files]
    rel_starts = [read_first_relative_timestamp(f) for f in timestamp_files]

    # Combine them to get absolute starts
    abs_starts = [u + r for u, r in zip(utc_starts, rel_starts)]

    # Find the latest absolute start => reference
    latest_start = max(abs_starts)

    # Calculate offsets
    offsets = {}
    for i, start in enumerate(abs_starts):
        offsets[i] = latest_start - start
    return offsets

def main():
    parser = argparse.ArgumentParser(
        description="Synchronize multiple audio files by trimming based on UTC start times + first relative timestamps."
    )
    parser.add_argument("--utc", nargs="+", required=True,
                        help="List of utc_start_stream_x.txt files, same order as timestamps & audio.")
    parser.add_argument("--timestamps", nargs="+", required=True,
                        help="List of timestamps_x.txt files, same order as utc & audio.")
    parser.add_argument("--audio", nargs="+", required=True,
                        help="List of audio_file_x.wav files, same order as utc & timestamps.")

    args = parser.parse_args()

    # Compute the required trim offsets
    offsets = calculate_offsets(args.utc, args.timestamps)

    # For each index, call adjust_audio
    for i, audio_in in enumerate(args.audio):
        # Build the synced output name
        base, ext = os.path.splitext(audio_in)
        audio_out = f"{base}_synced{ext}"

        offset_s = offsets[i]
        adjust_audio(audio_in, audio_out, offset_s)

    print("All audio files have been synced.")

if __name__ == "__main__":
    main()

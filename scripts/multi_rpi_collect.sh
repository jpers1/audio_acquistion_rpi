#!/bin/bash
#
# multi_rpi_collect.sh
# --------------------
# Collects & filters audio from remote Pis, syncs them, merges to multi-channel,
# then prompts user for a 4-digit code and creates a .tar (no gzip) named:
#    audiofiles-<CODE>-YYYY-MM-DD-HH-MM-SS.tar
# in ~/dataset/<FOLDERNAME>.
#
# Usage:
#   ./multi_rpi_collect.sh --folder <FOLDER_NAME>
#
# Requirements:
#   - remotes.conf listing your hosts
#   - ffmpeg on each remote + local
#   - Python 3 + numpy + soundfile for multi_rpi_sync.py
#   - Writes to /dev/shm/audiofiles on local Pi

################################################
# 0. PARSE ARGUMENTS
################################################

FOLDER_NAME=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --folder)
      FOLDER_NAME="$2"
      shift 2
      ;;
    *)
      echo "Unknown parameter: $1"
      exit 1
      ;;
  esac
done

if [ -z "$FOLDER_NAME" ]; then
  echo "Error: You must specify --folder <NAME>"
  exit 1
fi

################################################
# 1. SET DESTINATION DIRECTORY (LOCAL)
################################################

DESTINATION_DIR="/dev/shm/audiofiles"

# Clear out old contents
if [ -d "$DESTINATION_DIR" ]; then
  rm -rf "$DESTINATION_DIR"
fi
mkdir -p "$DESTINATION_DIR"

################################################
# 2. READ HOSTS FROM remotes.conf
################################################

HOST_LIST=()
if [ ! -f "remotes.conf" ]; then
  echo "Error: remotes.conf not found!"
  exit 1
fi

while IFS= read -r line; do
  [ -z "$line" ] && continue
  HOST_LIST+=("$line")
done < remotes.conf

if [ ${#HOST_LIST[@]} -eq 0 ]; then
  echo "Error: No hosts found in remotes.conf!"
  exit 1
fi

################################################
# 3. HELPER FUNCTIONS
################################################

cleanup_and_exit() {
  echo "An error occurred during file collection. Exiting..."
  exit 1
}

safe_scp_and_rename() {
  local HOST="$1"
  local REMOTE_FILE="$2"
  local PREFIX="$3"
  local EXT="$4"

  local SAFE_HOST
  SAFE_HOST="$(echo "$HOST" | sed 's/\./_/g')"

  local LOCAL_FILE="${DESTINATION_DIR}/${PREFIX}_${SAFE_HOST}.${EXT}"

  echo " - Transferring $REMOTE_FILE from $HOST to $LOCAL_FILE"
  scp "pi@${HOST}:${REMOTE_FILE}" "$LOCAL_FILE"
  if [ $? -ne 0 ]; then
    echo "Error: Failed to copy $REMOTE_FILE from $HOST"
    cleanup_and_exit
  fi
}

################################################
# 4. REMOTE HP FILTER (10Hz)
################################################

echo "Starting remote FFmpeg high-pass filtering at 10Hz (in parallel)..."

pids=()
i=0
for HOST in "${HOST_LIST[@]}"; do
  echo "Host: $HOST => FFmpeg in background..."
  ssh pi@"$HOST" '
    ffmpeg -hide_banner -loglevel error -y \
      -i /dev/shm/audio_file.wav \
      -af "highpass=f=10" \
      -c:a pcm_f32le -ar 44100 -ac 2 \
      /dev/shm/audio_file_hp_tmp.wav && \
    mv /dev/shm/audio_file_hp_tmp.wav /dev/shm/audio_file.wav
  ' 1>/dev/null 2>/dev/null &

  pids[$i]=$!
  ((i++))
done

for pid in "${pids[@]}"; do
  wait "$pid" || {
    echo "Error: One or more FFmpeg processes failed!"
    cleanup_and_exit
  }
done

echo "All remote FFmpeg filtering jobs completed."

################################################
# 5. COLLECT FILES
################################################

echo "Collecting data from each host..."

UTC_FILES=()
TIMESTAMP_FILES=()
AUDIO_FILES=()

for HOST in "${HOST_LIST[@]}"; do
  echo "Host: $HOST"

  localSafeHost="$(echo "$HOST" | sed 's/\./_/g')"

  # 1) timestamps
  safe_scp_and_rename "$HOST" "/dev/shm/timestamps.txt" "timestamps" "txt"
  TIMESTAMP_FILES+=("${DESTINATION_DIR}/timestamps_${localSafeHost}.txt")

  # 2) audio_file
  safe_scp_and_rename "$HOST" "/dev/shm/audio_file.wav" "audio_file" "wav"
  AUDIO_FILES+=("${DESTINATION_DIR}/audio_file_${localSafeHost}.wav")

  # 3) utc_start_stream
  safe_scp_and_rename "$HOST" "/dev/shm/utc_start_stream.txt" "utc_start_stream" "txt"
  UTC_FILES+=("${DESTINATION_DIR}/utc_start_stream_${localSafeHost}.txt")

  # Remove remote files
  ssh pi@"$HOST" "rm /dev/shm/timestamps.txt /dev/shm/audio_file.wav /dev/shm/utc_start_stream.txt" 2>/dev/null

  echo "Done collecting from $HOST"
  echo "-----------------------------------"
  sleep 0.5
done

echo "All files collected."

################################################
# 6. SYNC FILES
################################################

echo "Synchronizing audio files..."
python3 multi_rpi_sync.py \
  --utc "${UTC_FILES[@]}" \
  --timestamps "${TIMESTAMP_FILES[@]}" \
  --audio "${AUDIO_FILES[@]}"

if [ $? -eq 0 ]; then
  echo "Audio synchronization complete."
  echo "Synced files in $DESTINATION_DIR (with *_synced.wav)."
else
  echo "Error: Audio synchronization failed."
  cleanup_and_exit
fi

################################################
# 7. MERGE 4 STEREO => 8CH, 4CH LEFT, 4CH RIGHT
################################################

SYNCED_FILES=( "${DESTINATION_DIR}"/*_synced.wav )
if [ ${#SYNCED_FILES[@]} -eq 4 ]; then
  echo "Exactly 4 synced files found. Checking if each is stereo..."
  STEREO_COUNT=0
  VALID_FILES=()

  for f in "${SYNCED_FILES[@]}"; do
    CHANNELS=$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$f")
    if [ "$CHANNELS" -eq 2 ]; then
      ((STEREO_COUNT++))
      VALID_FILES+=("$f")
    else
      echo "'$f' not stereo; skipping merges."
    fi
  done

  if [ "$STEREO_COUNT" -eq 4 ]; then
    echo "All 4 are stereo. Merging..."

    # 7a: merged_8ch.wav
    ffmpeg -y \
      -i "${VALID_FILES[0]}" \
      -i "${VALID_FILES[1]}" \
      -i "${VALID_FILES[2]}" \
      -i "${VALID_FILES[3]}" \
      -filter_complex "[0:a][1:a][2:a][3:a]join=inputs=4:channel_layout=7.1[a]" \
      -map "[a]" -c:a pcm_f32le "${DESTINATION_DIR}/merged_8ch.wav"

    # 7b: merged_4ch_left.wav
    ffmpeg -y \
      -i "${VALID_FILES[0]}" \
      -i "${VALID_FILES[1]}" \
      -i "${VALID_FILES[2]}" \
      -i "${VALID_FILES[3]}" \
      -filter_complex "
        [0:a]channelsplit=channel_layout=stereo:channels=FL[left0];
        [1:a]channelsplit=channel_layout=stereo:channels=FL[left1];
        [2:a]channelsplit=channel_layout=stereo:channels=FL[left2];
        [3:a]channelsplit=channel_layout=stereo:channels=FL[left3];
        [left0][left1][left2][left3]join=inputs=4:channel_layout=quad[out]
      " \
      -map "[out]" -c:a pcm_f32le "${DESTINATION_DIR}/merged_4ch_left.wav"

    # 7c: merged_4ch_right.wav
    ffmpeg -y \
      -i "${VALID_FILES[0]}" \
      -i "${VALID_FILES[1]}" \
      -i "${VALID_FILES[2]}" \
      -i "${VALID_FILES[3]}" \
      -filter_complex "
        [0:a]channelsplit=channel_layout=stereo:channels=FR[right0];
        [1:a]channelsplit=channel_layout=stereo:channels=FR[right1];
        [2:a]channelsplit=channel_layout=stereo:channels=FR[right2];
        [3:a]channelsplit=channel_layout=stereo:channels=FR[right3];
        [right0][right1][right2][right3]join=inputs=4:channel_layout=quad[out]
      " \
      -map "[out]" -c:a pcm_f32le "${DESTINATION_DIR}/merged_4ch_right.wav"

    echo "Merges done."
  fi
fi

################################################
# 8. PROMPT FOR 4-DIGIT CODE => TAR
################################################

while true; do
  echo -n "Enter up to 4 digits in [0-4] (e.g. '1234'), empty => '0000': "
  read -r USER_CODE
  USER_CODE="$(echo "$USER_CODE" | tr -d '[:space:]')"

  if [ -z "$USER_CODE" ]; then
    USER_CODE="0000"
  fi

  if [ ${#USER_CODE} -gt 4 ]; then
    echo "Too many digits. Please enter at most 4."
    continue
  fi

  # Pad trailing zeros
  while [ ${#USER_CODE} -lt 4 ]; do
    USER_CODE="${USER_CODE}0"
  done

  # Validate each char
  VALID=true
  for (( i=0; i<4; i++ )); do
    c="${USER_CODE:$i:1}"
    case "$c" in
      [0-4]) ;;
      *)
        echo "Invalid digit '$c'. Must be [0-4]."
        VALID=false
        break
        ;;
    esac
  done

  if $VALID; then
    break
  fi
done

echo "Final code: $USER_CODE"

TIMESTAMP=$(date +%Y-%m-%d-%H-%M-%S)
TAR_DEST="$HOME/dataset/$FOLDER_NAME"
mkdir -p "$TAR_DEST"

TAR_FILENAME="audiofiles-${USER_CODE}-${TIMESTAMP}.tar"
echo "Creating tar => $TAR_DEST/$TAR_FILENAME"

tar -cvf "${TAR_DEST}/${TAR_FILENAME}" -C /dev/shm audiofiles
if [ $? -eq 0 ]; then
  echo "Tar created: ${TAR_DEST}/${TAR_FILENAME}"
else
  echo "Error creating tar!"
fi

echo "All done."
exit 0

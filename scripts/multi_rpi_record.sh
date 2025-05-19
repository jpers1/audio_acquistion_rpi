#!/bin/bash
#
# multi_rpi_record.sh
# -------------------
# This script repeatedly starts an audio recording program on multiple Raspberry Pis
# for a specified duration, then calls multi_rpi_collect.sh to filter/sync/merge/tar
# the recordings. You can continue recording new sessions or quit.
#
# Usage:
#   ./multi_rpi_record.sh <record_duration_in_seconds> [folder_name]
#
# Examples:
#   ./multi_rpi_record.sh 10
#   ./multi_rpi_record.sh 10 myfolder
#
# If folder_name is not specified, it defaults to "defaultfolder".
# The final tar file is named: audiofiles-<CODE>-YYYY-MM-DD-HH-MM-SS.tar
# and goes into ~/dataset/<folder_name>/.

########################################
# 1. PARSE COMMAND-LINE ARGUMENTS
########################################

if [ $# -lt 1 ]; then
  echo "Usage: $0 <record_duration_in_seconds> [folder_name]"
  exit 1
fi

RECORD_DURATION=$1
FOLDER_NAME=$2

if [ -z "$FOLDER_NAME" ]; then
  FOLDER_NAME="defaultfolder"
fi

########################################
# 2. MAIN LOOP
########################################

while true; do

  ################################################
  # 2a. READ HOSTS FROM remotes.conf
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
    echo "No hostnames/IPs found in remotes.conf. Exiting!"
    exit 1
  fi

  ################################################
  # 2b. START RECORDING ON ALL HOSTS
  ################################################
  AUDIO_PROGRAM="basis_audio_program_adctimestamps"
  PROGRAM_PATH="/home/pi/bin"

  kill_audio_program_on_all_hosts() {
    echo "Terminating audio program on all hosts..."
    for HOSTNAME in "${HOST_LIST[@]}"; do
      ssh pi@"$HOSTNAME" "pkill -f -2 $(basename "$AUDIO_PROGRAM")" 2>/dev/null
      echo "Terminated audio program on $HOSTNAME."
    done
  }

  cleanup_and_exit() {
    echo "Cleaning up..."
    kill_audio_program_on_all_hosts
    exit 1
  }

  echo "Starting audio program on all hosts..."
  for HOSTNAME in "${HOST_LIST[@]}"; do
    echo "  -> $HOSTNAME"
    ssh pi@"$HOSTNAME" "cd $PROGRAM_PATH && nohup ./$AUDIO_PROGRAM >/dev/null 2>&1 &" &
    sleep 0.5

    # Check if process started successfully
    if ssh pi@"$HOSTNAME" "pgrep -f $AUDIO_PROGRAM > /dev/null 2>&1"; then
      echo "    Audio program successfully started on $HOSTNAME."
    else
      echo "    Failed to start audio program on $HOSTNAME."
      cleanup_and_exit
    fi
  done

  echo "All hosts are now recording for $RECORD_DURATION seconds..."
  sleep "$RECORD_DURATION"

  # Stop the recording
  kill_audio_program_on_all_hosts
  echo "Recording completed."
  sleep 3

  ################################################
  # 2c. CALL multi_rpi_collect.sh
  ################################################
  SCRIPT_DIR="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"
  cd "$SCRIPT_DIR" || cleanup_and_exit

  echo "Starting data collection (folder=$FOLDER_NAME) ..."
  ./multi_rpi_collect.sh --folder "$FOLDER_NAME"
  if [ $? -eq 0 ]; then
    echo "Data collection completed successfully."
  else
    echo "Data collection failed."
    cleanup_and_exit
  fi

  echo "Finished one full cycle (record + collect)."

  ################################################
  # 2d. PROMPT user to do another or quit
  ################################################
  echo
  read -n1 -r -p "Press [ENTER] to record another session, or 'q' to quit: " KEY
  echo
  if [[ "$KEY" == "q" || "$KEY" == "Q" ]]; then
    echo "Exiting the loop. Goodbye!"
    break
  fi

done

echo "All done. Exiting."
exit 0

# Audio Acquisition Project

This project implements the recording and timestamping of a continuous audio stream using the PortAudio API on Unix system. 
Several recording methods are  available, with the most comprehensive functionality provided by the `basis_audio_program_adctimestamps.c` file. 
The file `pa_devs.c` lists all recognized audio hardware on the system. It is recommended to run this file before any recording session to verify that your system has correctly recognized the digital microphone. 

## Prerequisites

- Build each library from the lib directory, and then proceed to build the project. More documentation is in the Notion documentation.

## Setup Instructions

### 1. Clone the Project

First, download or clone the project repository to your local machine:

```bash
git clone https://github.com/emina-ferzana/audio_project_rpi.git
cd audio_project_rpi
mkdir build
cd build
Cmake ..
make
```

### 2. Run the Executables

To run the prepared programs, use the following command:

```bash
cd build/bin
./name_of_file
```

#### Further work

Please introduce and push all changes from audio2.lmi.link! This github repo is synced to the directory audio_project_rpi on pi@audio2.lmi.link. 
The other RPis can be updated using the script `deploy_binary.sh` from the directory: `remotetesting`.
Build everything on one RPi and send the appropriate binaries over the network to the other RPis.

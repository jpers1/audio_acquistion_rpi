
# Dependencies

This project relies on several libraries and tools to compile and run.  
Below is a summary of what you need on a Raspberry Pi OS (Raspbian) Lite installation.

## Prerequisites

1. **Build Essentials and CMake**

   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential cmake
   ```

   This installs GCC, Make, and CMake.

2. **PortAudio + ALSA**

   ```bash
   sudo apt-get install -y portaudio19-dev libasound2-dev
   ```

   These are needed for audio capture/playback (PortAudio) and ALSA support on Linux.

3. **Libsndfile**

   ```bash
   sudo apt-get install -y libsndfile1-dev
   ```

   Provides the `sndfile.h` header and libraries for reading and writing various audio file formats.

4. **FFTW**

   ```bash
   sudo apt-get install -y libfftw3-dev
   ```

   Needed for performing fast Fourier transforms.

5. **Optional: Serial Utilities**

   If you’re using a serial compass or other serial peripherals:
   ```bash
   sudo apt-get install -y minicom
   ```
   And ensure your user is in the `dialout` group:
   ```bash
   sudo usermod -a -G dialout <username>
   ```

## Building

1. Clone or download the repo.
2. Create and enter a build directory:
   ```bash
   mkdir build && cd build
   ```
3. Generate Makefiles with CMake:
   ```bash
   cmake ..
   ```
4. Compile:
   ```bash
   make
   ```
5. (Optional) Install:
   ```bash
   sudo make install
   ```

## Running

- The compiled binaries will appear in `build/bin` (depending on your CMake setup).  
- For example:
  ```bash
  ./bin/record_example
  ```
- Pass any required command-line arguments specific to each program.

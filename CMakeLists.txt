cmake_minimum_required(VERSION 3.10)
project(MyAudioProject)

# Set the C standard to C99
set(CMAKE_C_STANDARD 99)


# Specify the source files for the two programs
add_executable(pa_devs src/pa_devs.c)
add_executable(record_example src/record_example.c)
add_executable(test_fftw src/test_fftw.c)
add_executable(continuous_capture src/continuous_capture.c /home/pi/audio_project_rpi/lib/portaudio/src/common/pa_ringbuffer.c)
add_executable(paex_read_write_wire src/paex_read_write_wire.c)
add_executable(paex_pink src/paex_pink.c)
add_executable(thread_example src/thread_example.c)
add_executable(basis_audio_program src/basis_audio_program.c /home/pi/audio_project_rpi/lib/portaudio/src/common/pa_ringbuffer.c)
add_executable(basis_audio_program_processing src/basis_audio_program_processing.c /home/pi/audio_project_rpi/lib/portaudio/src/common/pa_ringbuffer.c)
add_executable(basis_audio_program_threads src/basis_audio_program_threads.c /home/pi/audio_project_rpi/lib/portaudio/src/common/pa_ringbuffer.c)

# Link the executables to the PortAudio library
target_link_libraries(pa_devs portaudio  asound m pthread rt)
target_link_libraries(record_example portaudio  asound m pthread rt)
target_link_libraries(test_fftw sndfile fftw3 m)
target_link_libraries(paex_read_write_wire portaudio asound)
target_link_libraries(paex_pink portaudio asound)
target_link_libraries(thread_example pthread)

# Add the directory where pa_ringbuffer.h is located to your include path
target_include_directories(continuous_capture PRIVATE "/home/pi/audio_project_rpi/lib/include" "/home/pi/audio_project_rpi/lib/portaudio/src/common")
target_include_directories(basis_audio_program PRIVATE "/home/pi/audio_project_rpi/lib/include" "/home/pi/audio_project_rpi/lib/portaudio/src/common")
target_include_directories(basis_audio_program_processing PRIVATE "/home/pi/audio_project_rpi/lib/include" "/home/pi/audio_project_rpi/lib/portaudio/src/common")
target_include_directories(basis_audio_program_threads PRIVATE "/home/pi/audio_project_rpi/lib/include" "/home/pi/audio_project_rpi/lib/portaudio/src/common")

target_link_libraries(continuous_capture portaudio asound pthread m)
target_link_libraries(basis_audio_program portaudio asound m)
target_link_libraries(basis_audio_program_processing portaudio asound m)
target_link_libraries(basis_audio_program_threads portaudio sndfile m pthread)

# Set the output directory for the binaries
set_target_properties(pa_devs
                    record_example
                    test_fftw 
                    continuous_capture
                    paex_read_write_wire
                    paex_pink
                    thread_example
                    basis_audio_program
		    basis_audio_program_processing
                    basis_audio_program_threads
                    PROPERTIES
                    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

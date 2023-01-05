# file-sink

A stripped down SFTP client with file synchronization. Automatically upload
local files to a remote server by modifying them.

https://user-images.githubusercontent.com/16218676/210679671-ff24ead9-9c9f-4cfa-a795-87d243868146.mp4

## Building

Install CMake and Visual Studio.

```sh
mkdir build
cd build
cmake ..

# development build
cmake --build .

# release build
cmake --build . --config Release
```
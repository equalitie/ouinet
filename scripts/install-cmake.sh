#!/bin/bash

CMAKE_VERSION=$1
INSTALL_DIR="/opt/cmake"

if [[ -z $CMAKE_VERSION ]]; then
  echo "Missing <CMAKE_VERSION> parameter."
  echo "Usage: ${0} <CMAKE_VERSION>"
  exit 1
fi

if [[ ! $CMAKE_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+-[^-]+-[^-]+$ ]]; then
  echo "Incorrect <CMAKE_VERSION> parameter."
  echo "Expected format: <MAJOR>.<MINOR>.<PATCH>-<OS>-<ARCH>"
  echo "Example: 3.28.0-linux-x86_64"
  exit 1
fi

IFS='.' read -r MAJOR MINOR PATCH <<< $(echo $CMAKE_VERSION | cut -d- -f 1 )

filename="cmake-"$CMAKE_VERSION
cd $INSTALL_DIR
echo "Downloading ${filename}..."
wget --quiet "https://cmake.org/files/v"$MAJOR"."$MINOR"/"$filename".tar.gz"

if [ $? -ne 0 ]; then
  echo "Failed to download the file from" \
       "https://cmake.org/files/v"$MAJOR"."$MINOR"/"$filename".tar.gz"
  exit 1
fi

echo "Decompressing ${filename}..."
tar -xzf $filename".tar.gz"

echo $INSTALL_DIR"/"$filename"/bin was added to the PATH"
export PATH=$INSTALL_DIR"/"$filename"/bin:"$PATH


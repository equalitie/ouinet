### Testing with Android emulator

You may also use the `build-android.sh` script to fire up an Android emulator
session with a compatible system image; just run:

    host $ /path/to/build-android.sh emu

It will download the necessary files to the current directory (or reuse files
downloaded by the build process, if available) and start the emulator.  Please
note that downloading the system image may take a few minutes, and booting the
emulator for the first time may take more than 10 minutes.  In subsequent
runs, the emulator will just recover the snapshot saved on last quit, which is
much faster.

The `ABI` environment variable described above also works for selecting the
emulator architecture:

    host $ env ABI=x86_64 /path/to/build-android.sh emu

You may also set `EMULATOR_API` to start a version of Android different from
the minimum one supported by Ouinet:

    host $ env EMULATOR_API=30 /path/to/build-android.sh emu  # Android 11

You may pass options to the emulator at the script's command line, after a
`--` (double dash) argument.  For instance:

    host $ /path/to/build-android.sh emu -- -no-snapshot-save

Some useful options include `-no-snapshot`, `-no-snapshot-load` and
`-no-snapshot-save`.  See [emulator startup options][] for more information.

[emulator startup options]: https://developer.android.com/studio/run/emulator-commandline.html#startup-options

While the emulator is running, you may interact with it using ADB, e.g. to
install the APK built previously.  See the script's output for particular
instructions and paths.

#### Running the Android emulator under Docker

The `Dockerfile.android-emu` file can be used to setup a Docker container able
to run the Android emulator.  First create the emulator image with:

    $ sudo docker build -t ouinet:android-emu - < Dockerfile.android-emu

Then, if `$SDK_PARENT_DIR` is the directory where you want Ouinet's build
script to place Android SDK downloads (so that you can reuse them between
container runs or from an existing Ouinet build), you may start a temporary
emulator container like this:

    $ sudo docker run --rm -it \
          --device /dev/kvm \
          --mount type=bind,source="$(realpath "$SDK_PARENT_DIR")",target=/mnt \
          --mount type=bind,source=$PWD,target=/usr/local/src,ro \
          --mount type=bind,source=/tmp/.X11-unix/X0,target=/tmp/.X11-unix/X0 \
          --mount type=bind,source=$HOME/.Xauthority,target=/root/.Xauthority,ro \
          -h "$(uname -n)" -e DISPLAY ouinet:android-emu

The `--device` option is only needed to emulate an `x86_64` device.

Please note how the Ouinet source directory as well as the X11 socket and
authentication cookie database are mounted into the container to allow showing
the emulator's screen on your display (without giving access to it to everyone
via `xhost` -- this is also why the container has the same host name as the
Docker host).

Once in the container, you may run the emulator like this:

    $ cd /mnt
    $ /usr/local/src/scripts/build-android.sh bootstrap emu &

You can use `adb` inside of the container to install packages into the
emulated device.


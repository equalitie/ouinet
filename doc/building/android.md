## Android library and demo client

Ouinet can also be built as an Android Archive library (AAR) to use in your
Android apps.

### Build requirements

A lot of free space (something less than 15 GiB).  Everything else shall be
downloaded by the `build-android.sh` script.

The instructions below use Vagrant for bulding, but the `build-android.sh`
script should work on any reasonably up-to-date Debian based system.

In the following instructions, we will use `<ANDROID>` to represent the
absolute path to your build directory.  That is, the directory from which you
will run the `build-android.sh` script (e.g. `~/ouinet.android.build`).

### Building

The following instructions will build a Ouinet AAR library and demo client
APK package for the `armeabi-v7a` [Android ABI][]:

    host    $ vagrant up --provider=libvirt
    host    $ vagrant ssh
    vagrant $ mkdir <ANDROID>
    vagrant $ cd <ANDROID>
    vagrant $ git clone --recursive /vagrant
    vagrant $ ./vagrant/scripts/build-android.sh

Note that we cloned a fresh copy of the Ouinet repository at `/vagrant`.  This
is not strictly necessary since the build environment supports out-of-source
builds, however it spares you from having to keep your source directory clean
and submodules up to date at the host.  If you fullfill these requirements,
you can just skip the cloning and run `/vagrant/scripts/build-android.sh`
instead.

If you want a build for a different ABI, do set the `ABI` environment
variable:

    vagrant $ env ABI=x86_64 /path/to/build-android.sh

In any case, when the build script finishes successfully, it will leave the
Ouinet AAR library at `build.ouinet/build-android-$ABI/builddir/ouinet/build-android/outputs/aar/ouinet-debug.aar`.

[Android ABI]: https://developer.android.com/ndk/guides/abis.html

#### Using existing Android SDK/NDK and Boost

By default the `build-android.sh` script downloads all dependencies required
to build the Ouinet Android library, including the Android SDK and NDK.  If
you already have these installed on your system you can tune the script to use
them:

    $ export SDK_DIR=/opt/android-sdk
    $ export NDK_DIR=/opt/android-sdk/ndk-bundle
    $ export ABI=armeabi-v7a
    $ /path/to/build-android.sh


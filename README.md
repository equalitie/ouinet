<img src="https://ouinet.work/img/ouinet-logo.png" width=250px alt="Ouinet">

[![pipeline status](https://gitlab.com/equalitie/ouinet/badges/main/pipeline.svg)](https://gitlab.com/equalitie/ouinet/commits/main)
[![release](https://gitlab.com/equalitie/ouinet/-/badges/release.svg)](https://gitlab.com/equalitie/ouinet/-/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-brightgreen.svg)](./LICENSE)

Ouinet is a Free/Open Source collection of software tools and support
infrastructure that provide access to web resources when access to the
unrestricted internet is unreliable or unavailable, tailored for scenarios of
limited Internet connectivity and selective network traffic filtering.

A typical *client* node setup consists of a web browser or other application
using the special HTTP proxy provided by Ouinet. When the Ouinet proxy gets
a request for content, it attempts to retrieve the resource using several
mechanisms. For example, it could try to fetch a page from the *distributed
cache* by looking up the content in the [BitTorrent DHT][] and if the content
is not available, it could also contact a trusted *injector* server over a
peer-to-peer routing system (like the [BitTorrent DHT][] or [I2P][]) and ask
to fetch the page and store it in the distributed cache.

See our [lightning talk at the Decentralized Web Summit 2018][] for an
overview of Ouinet's architecture or check the [documentation website][].

For a detailed technical explanation of processes and protocols you can refer
to [Ouinet's white paper][].

[lightning talk at the Decentralized Web Summit 2018]: http://archive.org/details/dweb-8_2_18_Lightning_Talks_New_Discoveries_5?start=547
[documentation website]: https://ouinet.work/docs/how/index.html
[Ouinet's white paper]: https://gitlab.com/equalitie/ouinet/-/blob/main/doc/ouinet-network-whitepaper.md?ref_type=heads
[BitTorrent DHT]: http://bittorrent.org/beps/bep_0005.html
[I2P]: https://geti2p.net/ "Invisible Internet Project"

![Ouinet request/response flow](./doc/request-response-flow.svg)

**Warning:** Ouinet is **not an anonymity tool**, information about your
browsing might be seen by other participants in the network, as well as the
fact that your application is seeding particular content.  Running some
components (like injector code) may turn your computer into an open web proxy,
and other security or privacy-affecting issues might exist.  Please keep this
in mind when using this software and only assume reasonable risks.

## Ouinet Components

* **Client**: Command line application that serves as a proxy to the Ouinet
network.
* **Injector**: Command line application that retrieves content from origin
websites and injects the signed content to the Ouinet network so it can be
shared peer-to-peer.
* **Android libraries**: Java Native Interface used to expose the C++ Ouinet
networking libraries to the Android applications written in Java or Kotlin.


## Cloning the source tree

Ouinet uses Git submodules, thus to properly clone it, use:

    $ git clone --recursive https://gitlab.com/equalitie/ouinet.git

You can also clone and update the modules separately:

    $ git clone https://gitlab.com/equalitie/ouinet.git
    $ cd ouinet
    $ git submodule update --init --recursive

## Build requirements (desktop)

To build Ouinet natively on your system, you will need the following software
to be already available:

* CMake 3.5+
* `g++` capable of C++14
* The [Boost library](http://www.boost.org/) 1.71+

Assuming that `<SOURCE DIR>` points to the directory where the
`CMakeLists.txt` file is, and `<BUILD DIR>` is a directory of your choice
where all (even temporary) build files will go, you can build Ouinet with:

    $ mkdir -p <BUILD DIR>
    $ cd <BUILD DIR>
    $ cmake <SOURCE DIR>
    $ make


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

### Integrating the Ouinet library into your app

In order for your Android app to access the resources it needs using the HTTP
protocol over Ouinet, thus taking advantage of its caching and distributed
request handling, you need to take few simple steps.

Here we assume that the app is developed in the Android Studio environment,
and that `<PROJECT DIR>` is your app's project directory.

#### Option A: Get Ouinet from Maven Central

Select the Ouinet version according to your app's ABI (we officially support
`ouinet-armeabi-v7a`, `ouinet-arm64-v8a` and `omni` that includes all the
supported ABIs plus `x86_64`), and also add Relinker as adependency in
`<PROJECT DIR>/app/build.gradle`:

```groovy
dependencies {
    //...
    implementation 'ie.equalit.ouinet:ouinet-armeabi-v7a:0.20.0'
    implementation 'com.getkeepsafe.relinker:relinker:1.4.4'
}
```

Check that Maven Central is added to the list of repositories used by
Gradle:

```groovy
allprojects {
    repositories {
        // ...
        mavenCentral()
    }
}
```

Now the Ouinet library will be automatically fetched by Gradle when your app is built.

#### Option B: Use your own compiled version of Ouinet

First, you need to compile the Ouinet library for the ABI environment you are
aiming at (e.g. `armeabi-v7a` or `x86_64`) as described above.  After the
`build_android.sh` script finishes successfully, you can copy the
`ouinet-debug.aar` file to your app libs folder:

```sh
$ cp /path/to/ouinet-debug.aar <PROJECT DIR>/app/libs/
```

Then look for the following section of your `<PROJECT DIR>/build.gradle`:

```groovy
allprojects {
  repositories {
    // ...
  }
}
```

And add this:

```groovy
flatDir {
  dirs 'libs'
}
mavenCentral()  // for ReLinker
```

Then look for the following section of your `<PROJECT DIR>/app/build.gradle`:

```groovy
dependencies {
  // ...
}
```

And add these:

```groovy
implementation 'com.getkeepsafe.relinker:relinker:1.4.4'
implementation(name:'ouinet-debug', ext:'aar')
```

#### Initialize Ouinet

At this stage your project should compile with no errors.  Now to tell Ouinet
to take over the app's HTTP communications, in the `MainActivity.java` of your
app import Ouinet:

```java
import ie.equalit.ouinet.Ouinet;
```

Then add a private member to your `MainActivity` class:

```java
private Ouinet ouinet;
```

And in its `OnCreate` method initiate the Ouinet object (using the BEP5/HTTP
cache):

```java
Config config = new Config.ConfigBuilder(this)
            .setCacheType("bep5-http")
            .setCacheHttpPubKey(<CACHE_PUB_KEY>)
            .setInjectorCredentials(<INJECTOR_USERNAME>:<INJECTOR_PASSWORD>)
            .setInjectorTlsCert(<INJECTOR_TLS_CERT>)
            .setTlsCaCertStorePath(<TLS_CA_CERT_STORE_PATH>)
            .build()

ouinet = new Ouinet(this, config);
ouinet.start();
```

From now on, all of the app's HTTP communication will be handled by Ouinet.

Please note that if you plan to use a directory for Ouinet's static cache in
your application (by using `ConfigBuilder`'s `setCacheStaticPath()` and
`setCacheStaticContentPath()`), then besides the permissions declared by the
library in its manifest, your app will need the `READ_EXTERNAL_STORAGE`
permission (Ouinet will not attempt to write to that directory).

#### Integration Examples

You can find additional information and samples of Android applications using
Ouinet in the following repository:
[equalitie/ouinet-examples](https://gitlab.com/equalitie/ouinet-examples).

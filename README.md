[![CircleCI](https://circleci.com/gh/equalitie/ouinet/tree/master.svg?style=shield)](https://circleci.com/gh/equalitie/ouinet/tree/master) [![pipeline status](https://gitlab.com/equalitie/ouinet/badges/master/pipeline.svg)](https://gitlab.com/equalitie/ouinet/commits/master)


# Ouinet

See [lightning talk at the Decentralized Web Summit 2018](http://archive.org/details/dweb-8_2_18_Lightning_Talks_New_Discoveries_5?start=547).

**Ouinet** is a Free/Open Source technology which allows web content to be
served with the help of an entire network of cooperating nodes using
peer-to-peer routing and distributed caching of responses.  This helps
mitigate the Web's characteristic single point of failure due to a client
application not being able to connect to a particular server.

The typical Ouinet *client* node setup consists of a web browser or other
application using a special HTTP proxy or API provided by a dedicated program
or library on the local machine.  When the client gets a request for content,
it attempts to retrieve the resource using several mechanisms.  It tries to
fetch the page from a *distributed cache* by looking up the content in a
*distributed cache index* (like the [BitTorrent][] DHT), and if not available,
it contacts a trusted *injector* server over a peer-to-peer routing system
(like [I2P][]) and asks it to fetch the page and store it in the distributed
cache.

[BitTorrent]: https://www.bittorrent.org/
[I2P]: https://geti2p.net/ "Invisible Internet Project"

![Ouinet request/response flow](./doc/request-response-flow.svg)

Future access by client nodes to popular content inserted in distributed
storage shall benefit from increased redundancy and locality, which
translates to: increased availability in the face of connectivity problems;
increased transfer speeds in case of poor upstream links; and reduced
bandwidth costs when internet access providers charge more for external or
international traffic.  Content injection is also designed to
allow for content re-introduction and seeding in extreme cases of total
connectivity loss (e.g. natural disasters).

The Ouinet library is a core technology that can be used by any application to
benefit from these advantages.  Ouinet integration provides any content
creator the opportunity to use cooperative networking and storage for the
delivery of their content to users around the world.

**Warning:** Ouinet is still **highly experimental**.  Some features (like
peer-to-peer routing) may or may not not work smoothly depending on the
different back-end technologies, and random unexpected crashes may occur.
Also, Ouinet is **not an anonymity tool**: information about your browsing
might be leaked to other participants in the network, as well as the fact that
your application is seeding particular content.  Running some components (like
injector code) may turn your computer into an open web proxy, and other
security or privacy-affecting issues might exist.  Please keep this in mind
when using this software and only assume reasonable risks.

**Note:** The steps described below have only been tested to work on GNU/Linux
on AMD64 platforms.  Building and testing Ouinet on your computer requires
familiarity with the command line.  At the moment there are no user-friendly
packages for Ouinet on the desktop.

# Table of Contents
- [Deployment with Docker](#deploy-a-client-or-injector-with-docker)
    - [Deploying a Client](#deploying-a-client)
    - [Using the Shell Container](#using-the-shell-container)
    - [Deploying an Injector](#deploying-an-injector)
    - [Other Deployments (eg; multiple clients/injectors)](#other-deployments)
    - [Building the Image](#building-the-image)
- [Android Library and Demo Client](#android-library-and-demo-client)
    - [Building](#building)
    - [Testing with Android emulator](#testing-with-android-emulator)
    - [Integrating the Ouinet Library Into Your App](#integrating-the-ouinet-library-into-your-app)
- [Testing (desktop)](#testing-desktop)
    - [Running a Test Injector](#running-a-test-injector)
    - [Running a Test Client](#running-a-test-client)
    - [Testing with a Browser](#testing-the-client-with-a-browser)
- [Setup a Development Environment](#development-environment)
    - [Building from Source](#building-from-source)
    - [Docker Development Environment](#docker-development-environment)
    - [Vagrant Development Environment](#vagrant-development-environment)

## Deploy a client or injector with Docker 

Ouinet injectors and clients can be run as Docker containers.  Ouinet Docker images are available on the [Docker Hub](https://hub.docker.com/equalitie/ouinet).  An application configuration file for Docker Compose is included for easily deploying all needed volumes and containers.

To run a Ouinet node container only a couple hundred MiB are needed, plus the space devoted to the data volume (which may grow considerably larger in the case of the injector).

A `Dockerfile` is also included that can be used to create a Docker image which contains the Ouinet injector, client and necessary software dependencies running on top of a Debian-based system.

### Deploying a Client

You may use [Docker Compose](https://docs.docker.com/compose/) with the
`docker-compose.yml` file included in Ouinet's source code (or you can just
[download it][docker-compose.yml]).  Whenever you run `docker-compose`
commands using that configuration file, **you must be in the directory where the
file resides**.

If you just plan to **run a single client** with the latest code on your
computer, you should be fine with running the following command:

    $ sudo docker-compose up

That command will create a *data volume*, a main *node container* for running
the Ouinet client or injector (using the host's network directly), and a
convenience *shell container* (see below) to allow you to modify files in the
data volume.  It will then run the containers (the shell container will exit
immediately; this is normal).

To **stop the node**, hit Ctrl+C or run `sudo docker-compose stop`.  Please
note that with the default configuration in `docker-compose.yml`, the node
will be automatically restarted whenever it crashes or the host is rebooted,
until explicitly stopped.

A new client node which starts with no configuration will get a default one
from templates included in Ouinet's source code and it will be missing some
important parameters, so you may want to stop it (see above) and use the
[shell container](#using-the-shell-container) to edit `client/ouinet-client.conf`:

  - If using a local test injector, set its endpoint in option `injector-ep`.
  - Set the injector's credentials in option `injector-credentials`.
  - Unless using a local test injector, set option `injector-tls-cert-file` to
    `/var/opt/ouinet/client/ssl-inj-cert.pem` and copy the injector's TLS
    certificate to that file.
  - Set the public key used by the injector for HTTP signatures in option
    `cache-http-public-key`.
  - To enable the distributed cache, set option `cache-type`.  The only value
    currently supported is `bep5-http`.

After you have set up your client's configuration, you can **restart it**.
The client's HTTP proxy endpoint should be available to the host at
`localhost` port 8077.

If you get a "connection refused" error when using the client's proxy, your
Docker setup may not support host networking.  To enable port forwarding,
follow the instructions in `docker-compose.yml`.

Finally, restart the client container.

[docker-compose.yml]: https://raw.githubusercontent.com/equalitie/ouinet/master/docker-compose.yml

### Using the shell container

You may use the convenience *shell container* to access Ouinet node files
directly:

    $ sudo docker-compose run --rm shell

This will create a throwaway container with a shell at the `/var/opt/ouinet`
directory in the data volume.

If you want to *transfer an existing repository* to `/var/opt/ouinet`, you
first need to move away or remove the existing one using the shell container:

    # mv REPO REPO.old  # REPO is either 'injector' or 'client'

Then you may copy it in from the host using:

    $ sudo docker cp /path/to/REPO SHELL_CONTAINER:/var/opt/ouinet/REPO

### Deploying an Injector

After an injector has finished starting, you may want to use the shell
container to inspect and note down the contents of `injector/endpoint-*`
(injector endpoints) and `injector/ed25519-public-key` (public key for HTTP
signatures) to be used by clients.  The injector will also generate a
`tls-cert.pem`  file which you should distribute to clients for TLS access.
Other configuration information like credentials can be found in
`injector/ouinet-injector.conf`.

To start the injector in headless mode, you can run:

    $ sudo docker-compose up -d

You will need to use `sudo docker-compose stop` to stop the container.

To be able to follow its logs, you can run:

    $ sudo docker-compose logs --tail=100 -ft
    
### Other deployments

If you plan on running several nodes on the same host you will need to use
different explicit Docker Compose project names for them.  To make the node an
injector instead of a client you need to set `OUINET_ROLE=injector`.  To make
the container use a particular image version instead of `latest`, set
`OUINET_VERSION`.  To limit the amount of memory that the container may use,
set `OUINET_MEM_LIMIT`, but you will need to pass the `--compatibility` option
to `docker-compose`.

An easy way to set all these parameters is to copy or link the
`docker-compose.yml` file to a directory with the desired project name and
populate its default environment file:

    $ mkdir -p /path/to/ouinet-injector  # ouinet-injector is the project name
    $ cd /path/to/ouinet-injector
    $ cp /path/to/docker-compose.yml .
    $ echo OUINET_ROLE=injector >> .env
    $ echo OUINET_VERSION=v0.1.0 >> .env
    $ echo OUINET_MEM_LIMIT=6g >> .env
    $ sudo docker-compose --compatibility up

### Building the image

If you still want to build the image yourself, follow the instructions in this section. You will need around 3 GiB of disk space.

You may use the `Dockerfile` as included in Ouinet's source code, or you can just [download it][Dockerfile].  Then build the image by running:

    $ sudo docker build -t equalitie/ouinet:latest - < Dockerfile

That command will build a default recommended version, which you can override with `--build-arg OUINET_VERSION=<VERSION>`.

After a while you will get the `equalitie/ouinet:latest` image.  Then you may want to run `sudo docker prune` to free up the space taken by temporary builder images (which may amount to a couple of GiB).

[Dockerfile]: https://raw.githubusercontent.com/equalitie/ouinet/master/Dockerfile

#### Debugging-enabled image

You can also build an alternative version of the image where programs contain
debugging symbols and they are run under `gdb`, which shows a backtrace in
case of a crash.  Just add `--build-arg OUINET_DEBUG=yes` to the build
command.  We recommend that you use a different tag for these images
(e.g. `equalitie/ouinet:<VERSION>-debug`).

Depending on your Docker setup, you may need to change the container's
security profile and give it tracing capabilities.  For more information, see
[this thread](https://stackoverflow.com/q/35860527).

## Testing (desktop)

### Running a test injector

If you want to run your own injector for testing and you have a local build,
create a copy of the `repos/injector` repository template directory included
in Ouinet's source tree:

    $ cp -r <SOURCE DIR>/repos/injector /path/to/injector-repo

When using a Docker-based injector as described above, just run and stop it so
that it creates a default configuration for you.

You should now edit `ouinet-injector.conf` in the injector repository (for
Docker, use the shell container to edit `injector/ouinet-injector.conf`):

 1. Enable listening on loopback addresses:

        listen-tcp = ::1:7070

    For clients you may then use `127.0.0.1:7070` as the *injector endpoint*
    (IPv6 is not yet supported).

 2. Change the credentials to use the injector (use your own ones):

        credentials = injector_user:injector_password

    For clients you may use these as *injector credentials*.

All the steps above only need to be done once.

Finally, start the injector.  For the local build you will need to explicitly
point it to the repository created above:

    $ <BUILD DIR>/injector --repo /path/to/injector-repo
    ...
    [INFO] HTTP signing public key (Ed25519): <CACHE_PUB_KEY>
    ...

Note down the `<CACHE_PUB_KEY>` string in the above output since clients will
need it as the *public key for HTTP signatures*.  You may also find that value
in the `ed25519-public-key` file in the injector repository.

When you are done testing the Ouinet injector, you may shut it down by hitting
Ctrl+C.

### Running a test client

To perform some tests using a Ouinet client and an existing test injector, you
first need to know the *injector endpoint* and *credentials*, its *TLS
certificate*, and its *public key for HTTP signatures*.  These use to be
respectively a `tcp:<IP>:<PORT>` string, a `<USER>:<PASSWORD>` string, a path
to a PEM file, and an Ed25519 public key (hexadecimal or Base32).

You need to configure the Ouinet client to use the aforementioned parameters.
If you have a local build, create a copy of the `repos/client` repository
template directory included in Ouinet's source tree:

    $ cp -r <SOURCE DIR>/repos/client /path/to/client-repo

When using a Docker-based client as described above, just run and stop it so
that it creates a default configuration for you.

Now edit `ouinet-client.conf` in the client repository (for Docker, use the
shell container to edit `client/ouinet-client.conf`) and add options for the
injector endpoint (if testing), credentials and public key.  Remember to
replace the values with your own:

    injector-ep = tcp:127.0.0.1:7070
    injector-credentials = injector_user:injector_password
    cache-http-public-key = 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
    cache-type = bep5-http

All the steps above only need to be done once.

Finally, start the client.  For the local build you will need to explicitly
point it to the repository created above:

    $ <BUILD DIR>/client --repo /path/to/client-repo

The client opens a web proxy on local port 8077 by default (see option
`listen-on-tcp` in its configuration file).  When you access the web using
this proxy (see the following section), your requests will go through your
local Ouinet client, which will attempt several mechanisms supported by Ouinet
to retrieve the resource.

When you are done testing the Ouinet client, you may shut it down by hitting
Ctrl+C.

### Testing the client with a browser

Once your local Ouinet client is running (see above), if you have Firefox
installed, you can create a new profile (stored under the `ff-profile`
directory in the example below) which uses the Ouinet client as an HTTP proxy
(listening on `localhost:8077` here) by executing the following commands on
another shell:

    mkdir -p ff-profile
    env http_proxy='http://localhost:8077/' firefox --no-remote --profile ff-profile

Otherwise you may manually [modify your browser's settings][Firefox proxy] to:

  - Make the client (listening on port `localhost:8077` here) its HTTP proxy
  - Make sure that `localhost` is not listed in the *No Proxy for* field
  - Check *Use this proxy for all protocols* (mostly for HTTPS)

[Firefox proxy]: http://www.wikihow.com/Enter-Proxy-Settings-in-Firefox
    "How to Enter Proxy Settings in Firefox"

Additionally, you may want to disable Firefox's automatic captive portal
detection so that you get less noise in client logs.  Enter `about:config` in
the location bar and change `network.captive-portal-service.enabled` to
`false`.

Once done, you can visit `localhost` in your browser and it should show you
the *client front-end* with assorted information from the client and
configuration tools:

  - To be able to browse HTTPS sites, you must first install the
    *client-specific CA certificate* linked from the top of the front-end page
    and authorize it to identify web sites.  Depending on your browser
    version, you may need to save it to disk first, then import it from
    *Preferences / Privacy & Security / Certificates / View Certificates…*
    into the *Authorities* list.

    The Ouinet client acts as a *man in the middle* to enable it to process
    HTTPS requests, but it (or a trusted injector when appropriate) still
    performs all standard certificate validations.  This CA certificate is
    unique to your device.

  - Several buttons near the top of the page look something like this:

        Injector access: enabled [ disable ]

    They allow you to enable or disable different *request mechanisms* to
    retrieve content:

      - *Origin*: The client contacts the origin server directly via HTTP(S).
      - *Proxy*: The client contacts the origin server through an HTTP proxy
        (currently the configured injector).
      - *Injector*: The client asks the injector to fetch and sign the content
        from the origin server, then it starts seeding the signed content to
        the distributed cache.
      - *Distributed Cache*: The client attempts to retrieve the content from
        the distributed cache.

    Content retrieved via the Origin and Proxy mechanisms is considered
    *private and not seeded* to the distributed cache.  Content retrieved via
    the Injector and Cache mechanisms is considered *public and seeded* to the
    distributed cache.

    These mechanisms are attempted in order according to a (currently
    hard-wired, customizable in the future) *request router configuration*.
    For instance, if one points the browser to a web page which is not yet
    in the distributed cache, then the client shall forward the request to the
    injector.  On success, (A) the injector will fetch, sign and send the
    content back to the client and (B) the client will seed the content to the
    cache.

  - Other information about the cache index is shown next.

**Note:** For a response to be injected, its request currently needs to carry
an `X-Ouinet-Group` header.  The [CENO Extension][] takes care of that
whenever browsing in normal mode, and it does not when browsing in private
mode.  Unfortunately, the Extension is not yet packaged independently and the
only way to use it is to clone its repository locally and load it every time
you start the browser; to do that, open Firefox's *Add-ons* window, then click
on the gears icon, then *Debug Add-ons*, then *Load Temporary Add-on…* and
choose the `manifest.json` file in the Extension's source tree.  Back to the
*Add-ons* page, remember to click on *CENO Extension* and allow *Run in
Private Windows* under *Details*.

[CENO Extension]: https://github.com/censorship-no/ceno-ext-settings/

After visiting a page with the Origin mechanism disabled and Injector
mechanism enabled, and waiting for a short while, you should be able to
disable all request mechanisms except for the Cache, clear the browser's
cached data, point the browser back to the same page and still get its
contents from the distributed cache even when the origin server is completely
unreachable.

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
to build the Ouinet Android library, including the Android SDK, Android NDK
and Boost for Android.  If you already have these installed on your system you
can tune the script to use them:

    $ export SDK_DIR=/opt/android-sdk
    $ export NDK_DIR=/opt/android-sdk/ndk-bundle
    $ export ABI=armeabi-v7a
    $ export PLATFORM=android-26
    $ export BOOST_V=1_67_0
    $ export BOOST_SOURCE=/path/to/Boost-for-Android
    $ export BOOST_INCLUDEDIR=$BOOST_SOURCE/build/out/${ABI}/include/boost-${BOOST_V}
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

You may pass options to the emulator at the script's command line, after a
`--` (double dash) argument.  For instance:

    host $ /path/to/build-android.sh emu -- -no-snapshot-save

Some useful options include `-no-snapshot`, `-no-snapshot-load` and
`-no-snapshot-save`.  See [emulator startup options][] for more information.

[emulator startup options]: https://developer.android.com/studio/run/emulator-commandline.html#startup-options

While the emulator is running, you may interact with it using ADB, e.g. to
install the APK built previously.  See the script's output for particular
instructions and paths.

### Integrating the Ouinet library into your app

In order for your Android app to access the resources it needs using the HTTP
protocol over Ouinet, thus taking advantage of its caching and distributed
request handling, you need to take few simple steps.

Here we assume that the app is developed in the Android Studio environment,
and that `<PROJECT DIR>` is your app's project directory.

First, you need to compile the Ouinet library for the ABI environment you are
aiming at (e.g. `armeabi-v7a` or `x86_64`) as described above.  After the
`build_android.sh` script finishes successfully, you can copy the
`ouinet-debug.aar` file to your app libs folder:

    $ cp /path/to/ouinet-debug.aar <PROJECT DIR>/app/libs/

Then look for the following section of your `<PROJECT DIR>/build.gradle`:

    allprojects {
      repositories {
        ...
      }
    }

And add this:

    flatDir {
      dirs 'libs'
    }

Then look for the following section of your `<PROJECT DIR>/app/build.gradle`:

    dependencies {
      ...
    }

And add this:

    implementation(name:'ouinet-debug', ext:'aar')

At this stage your project should compile with no errors.  Now to tell Ouinet
to take over the app's HTTP communications, in the `MainActivity.java` of your
app import Ouinet:

    import ie.equalit.ouinet.Ouinet;

Then add a private member to your `MainActivity` class:

    private Ouinet ouinet;

And in its `OnCreate` method initiate the Ouinet object (using the BEP5/HTTP
cache):

    Config config = new Config.ConfigBuilder(this)
                .setCacheType("bep5-http")
                .setCacheHttpPubKey(<CACHE_PUB_KEY>)
                .setInjectorCredentials(<INJECTOR_USERNAME>:<INJECTOR_PASSWORD>)
                .setInjectorTlsCert(<INJECTOR_TLS_CERT>)
                .setTlsCaCertStorePath(<TLS_CA_CERT_STORE_PATH>)
                .build()

    ouinet = new Ouinet(this, config);
    ouinet.start();

From now on all of the app's HTTP communication will be handled by Ouinet.

## Development Environment
### Building from source
#### Cloning the source tree

Ouinet uses Git submodules, thus to properly clone it, use:

    $ git clone --recursive https://github.com/equalitie/ouinet.git

You can also clone and update the modules separately:

    $ git clone https://github.com/equalitie/ouinet.git
    $ cd ouinet
    $ git submodule update --init --recursive

#### Build requirements (desktop)

To build Ouinet natively on your system, you will need the following software
to be already available:

* CMake 3.5+
* `g++` capable of C++14
* The [Boost library](http://www.boost.org/) 1.67+

Assuming that `<SOURCE DIR>` points to the directory where the
`CMakeLists.txt` file is, and `<BUILD DIR>` is a directory of your choice
where all (even temporary) build files will go, you can build Ouinet with:

    $ mkdir -p <BUILD DIR>
    $ cd <BUILD DIR>
    $ cmake <SOURCE DIR>
    $ make

However, we encourage you to use a Vagrant environment for development, or
Docker containers for deploying a Ouinet client or an injector.  These have a
different set of requirements.  See the corresponding sections below for
further instructions on Vagrant and Docker.

#### Running integration tests

The Ouinet source comes with a set of integration tests.  To run them you will
need the [Twisted](https://twistedmatrix.com/) Python framework.

If you already built Ouinet from `<SOURCE DIR>` into `<BUILD DIR>` (see
above), you can run the tests as follows:

    $ export OUINET_REPO_DIR=<SOURCE DIR>
    $ export OUINET_BUILD_DIR=<BUILD DIR>
    $ ./scripts/run_integration_tests.sh

### Docker Development Environment

We provide a *bootstrap* Docker image which is automatically updated with each
commit and provides all prerequisites for building the latest Oiunet desktop
binaries and Android libraries.

To exchange with the container data like Ouinet's source code and cached
downloads and build files, we will bind mount the following directories to
`/usr/local/src/` in the container (some we'll create first):

  - source (assumed to be at the current directory),
  - build (in `../ouinet.build/`),
  - and the container's `$HOME` (in `../ouinet.home/`), where `.gradle`,
    `.cargo`, etc. will reside.

Note that with the following incantations you will not be able to use `sudo`
in the container (`--user`), and that all the changes besides those in bind
mounts will be lost after you exit (`--rm`).

```sh
mkdir -p ../ouinet.build/ ../ouinet.home/
sudo docker run \
  --rm -it \
  --user $(id -u):$(id -g) \
  --mount type=bind,source="$(pwd)",target=/usr/local/src/ouinet \
  --mount type=bind,source="$(pwd)/../ouinet.build",target=/usr/local/src/ouinet.build \
  --mount type=bind,source="$(pwd)/../ouinet.home",target=/mnt/home \
  -e HOME=/mnt/home \
  registry.gitlab.com/equalitie/ouinet:android
```

If you only need to build Ouinet desktop binaries, you may replace the image
name at the end of the command with `registry.gitlab.com/equalitie/ouinet`,
which is much lighter.

After running the command, you should find yourself in a new terminal, ready
to accept the build instructions described elsewhere in the document.

Please consult the GitLab CI scripts to see how to build your own bootstrap
images locally.

### Vagrant Development Environment

One of the easiest ways to build Ouinet from source code (e.g. for development
or testing changes and fixes to code) is using a [Vagrant][] development
environment.

[Vagrant]: https://www.vagrantup.com/

To install Vagrant on a Debian system, run:

    $ sudo apt-get install vagrant

Ouinet's source tree contains a `Vagrantfile` which allows you to start a
Vagrant environment ready to build and run Ouinet by entering the source
directory and executing:

    $ vagrant up

If your Vagrant installation uses VirtualBox by default and you find problems,
you may need to force it to use libvirt instead:

    $ sudo apt-get install libvirt-bin libvirt-dev
    $ vagrant plugin install vagrant-libvirt
    $ vagrant up --provider=libvirt

#### Building Ouinet in Vagrant

Enter the Vagrant environment with `vagrant ssh`.  There you will find:

  - Your local Ouinet source tree mounted read-only under `/vagrant`
    (`<SOURCE DIR>` above).

  - Your local Ouinet source tree mounted read-write under `/vagrant-rw`.  You
    can use it as a bridge to your host.

  - `~vagrant/build-ouinet-git.sh`: Running this script will clone the Ouinet
    Git repository and all submodules into `$PWD/ouinet-git-source` and build
    Ouinet into `$PWD/ouinet-git-build` (`<BUILD DIR>` above).  Changes to
    source outside of the Vagrant environment will not affect this build.

  - `~vagrant/build-ouinet-local.sh`: Running this script will use your local
    Ouinet source tree (mounted under `/vagrant`) to build Ouinet into
    `$PWD/ouinet-local-build` (`<BUILD DIR>` above).  Thus you can edit source
    files on your computer and have them built in a consistent environment.

    Please note that this requires that you keep submodules in your checkout
    up to date as indicated above.

#### Accessing Ouinet services from your computer

The Vagrant environment is by default isolated, but you can configure it to
redirect ports from the host to the environment.

For instance, if you want to run a Ouinet client (with its default
configuration) in Vagrant and use it as a proxy in a browser on your computer,
you may uncomment the following line in `Vagrantfile`:

    #vm.vm.network "forwarded_port", guest: 8077, host: 8077, guest_ip: "127.0.0.1"

And restart the environment:

    $ vagrant halt
    $ vagrant up

Then you can configure your browser to use `localhost` port 8077 to contact
the HTTP proxy.

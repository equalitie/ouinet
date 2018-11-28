[![CircleCI](https://circleci.com/gh/equalitie/ouinet/tree/master.svg?style=shield)](https://circleci.com/gh/equalitie/ouinet/tree/master)

# Ouinet

**Ouinet** is a Free/Open Source technology which allows web content to be
served with the help of an entire network of cooperating nodes using
peer-to-peer routing and distributed caching of responses.  This helps
mitigate the Web's characteristic single point of failure due to a client
application not being able to connect to a particular server.

The typical Ouinet *client* node setup consists of a web browser or other
application using a special HTTP proxy or API provided by a dedicated program
or library on the local machine.  When the client gets a request for content,
it attempts to retrieve the resource using several mechanisms.  It tries to
fetch the page from a distributed cache (like [IPFS][]), and if not available,
it contacts a trusted *injector* server over a peer-to-peer routing system
(like [I2P][]) and asks it to fetch the page and store it in the *distributed
cache*.

[IPFS]: https://ipfs.io/ "InterPlanetary File System"
[I2P]: https://geti2p.net/ "Invisible Internet Project"

![Ouinet request/response flow](./doc/request-response-flow.svg)

Future accesses by client nodes to popular content inserted in distributed
storage shall benefit from an increased redundancy and locality, which
translates in increased availability in the face of connectivity problems,
increased transfer speeds in case or poor upstream links, and reduced
bandwidth costs when access providers charge more for external or
international traffic.  Content injection is also designed in a way which
allows for content re-introduction and seeding on extreme cases of total
connectivity loss (e.g. natural disasters).

The Ouinet library is a core technology that can be used by any application to
benefit from these advantages.  Ouinet integration provides any content
creator the opportunity to use cooperative networking and storage for the
delivery of their content to users around the world.

**Warning:** Ouinet is still highly experimental.  Some features (like
peer-to-peer routing) may or may not not work smoothly depending on the
different back end technologies.  Information about your browsing might be
leaked to other participants in the network.  Running some components (like
injector code) may turn your computer into an open web proxy, and other
security or privacy-affecting issues might exist.  Please keep this in mind
when using this software.

**Note:** The steps described below have only been tested to work on GNU/Linux
on AMD64 platforms.  Building and testing Ouinet on your computer requires
familiarity with the command line.  At the moment there are no user-friendly
packages for Ouinet on the desktop.

## Cloning the source tree

Ouinet uses Git submodules, thus to properly clone it, use:

    $ git clone --recursive https://github.com/equalitie/ouinet.git

You can also clone and update the modules separately:

    $ git clone https://github.com/equalitie/ouinet.git
    $ cd ouinet
    $ git submodule update --init --recursive

## Build requirements (desktop)

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

## Running integration tests

The Ouinet source comes with a set of integration tests.  To run them you will
need the [Twisted](https://twistedmatrix.com/) Python framework.

If you already built Ouinet from `<SOURCE DIR>` into `<BUILD DIR>` (see
above), you can run the tests as follows:

    $ export OUINET_REPO_DIR=<SOURCE DIR>
    $ export OUINET_BUILD_DIR=<BUILD DIR>
    $ ./scripts/run_integration_tests.sh

## Using a Vagrant environment

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

### Building Ouinet in Vagrant

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

### Accessing Ouinet services from your computer

The Vagrant environment is by default isolated, but you can configure it to
redirect ports from the host to the environment.

For instance, if you want to run a Ouinet client (with its default
configuration) in Vagrant and use it as a proxy in a browser on your computer,
you may uncomment the following line in `Vagrantfile`:

    #vm.vm.network "forwarded_port", guest: 8080, host: 8081, guest_ip: "127.0.0.1"

And restart the environment:

    $ vagrant halt
    $ vagrant up

Then you can configure your browser to use `localhost` port 8081 to contact
the HTTP proxy.

### Vagrant instance on AWS

The source tree also contains `Vagrantfile.aws`, which you can use to deploy
the Vagrant environment to Amazon Web Services (AWS):

    $ vagrant plugin install vagrant-aws
    $ vagrant plugin install vagrant-sshfs

    $ export AWS_ACCESS_KEY_ID='YOUR_ACCESS_ID'
    $ export AWS_SECRET_ACCESS_KEY='your secret token'

    $ VAGRANT_VAGRANTFILE=Vagrantfile.aws vagrant up
    $ vagrant sshfs --mount linux
    $ vagrant ssh

## Using Docker containers

Ouinet injectors and clients can be run as Docker containers.  An application
configuration file for Docker Compose is included for easily deploying all
needed volumes and containers.

To run a Ouinet node container only a couple hundred MiB are needed, plus the
space devoted to the data volume (which may grow considerably larger in the
case of the injector).

A `Dockerfile` is also included that can be used to create a Docker image
which contains the Ouinet injector, client and necessary software dependencies
running on top of a Debian base system.

### Building the image

Ouinet Docker images should be available from the Docker Hub.  Follow the
instructions in this section if you still want to build the image yourself.
You will need around 3 GiB of disk space.

You may use the `Dockerfile` as included in Ouinet's source code, or you
can just [download it][Dockerfile].  Then build the image by running:

    $ sudo docker build -t equalitie/ouinet:latest .

That command will build a default recommended version, which you can override
with `--build-arg OUINET_VERSION=<VERSION>`.

After a while you will get the `equalitie/ouinet:latest` image.  Then you may
want to run `sudo docker prune` to free up the space taken by temporary
builder images (which may amount to a couple of GiB).

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

### Deploying a client

You may use [Docker Compose](https://docs.docker.com/compose/) with the
`docker-compose.yml` file included in Ouinet's source code (or you can just
[download it][docker-compose.yml]).  Whenever you run `docker-compose`
commands using that configuration file, you must be at the directory where the
file resides.

If you just plan to **run a single client** with the latest code on you
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
**shell container** (see below) to edit `client/ouinet-client.conf`:

  - Add configuration options for the injector endpoint `injector-ep` and its
    access credentials `injector-credentials`.
  - If the injector endpoint uses TLS, set `enable-injector-tls` to `true` and
    copy its certificate to `client/ssl-inj-cert.pem`.
  - Set the IPNS ID of the cache index in option `injector-ipns`.

After you have set up your client's configuration, you can **restart it**.
The client's HTTP proxy endpoint should be available to the host at
`127.7.2.1` port 8080.

If you get a "connection refused" error when using the client's proxy, your
Docker setup may not support host networking.  To enable port forwarding,
follow the instructions in `docker-compose.yml` and use the shell container
(see below) to run this command and thus modify `listen-on-*` options:

    # sed -i 's/127.7.2.1/0.0.0.0/' */ouinet-*.conf

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

### Other deployments

If you plan on running several nodes on the same host you will need to use
different explicit Docker Compose project names for them.  To make the node an
injector instead of a client you need to set `OUINET_ROLE=injector`.  To make
the container use a particular image version instead of `latest`, set
`OUINET_VERSION`.

An easy way to set all these parameters is to copy or link the
`docker-compose.yml` file to a directory with the desired project name and
populate its default environment file:

    $ mkdir -p /path/to/ouinet-injector  # ouinet-injector is the project name
    $ cd /path/to/ouinet-injector
    $ cp /path/to/docker-compose.yml .
    $ echo OUINET_ROLE=injector >> .env
    $ echo OUINET_VERSION=v0.0.5-docker3 >> .env
    $ sudo docker-compose up

### Injector container

After an injector has finished starting, you may want to use the shell
container to inspect and note down the contents of `injector/endpoint-*`
(injector endpoints) and `injector/cache-ipns` (cache index IPNS ID) to be
used by clients.  The injector will also generate a `tls-cert.pem` file which
you should distribute to clients for TLS access.

To start the injector in headless mode, you can run:

    $ sudo docker-compose up -d

You will need to use `sudo docker-compose stop` to stop the container.

To be able to follow its logs, you can run:

    $ sudo docker-compose logs --tail=100 -ft

If you ever need to reset and empty the injector's cache index for some reason
(e.g. testing) while keeping injector IDs and credentials, you may:

 1. Fetch a Go IPFS binary and copy it to the data volume:

        $ wget "https://dist.ipfs.io/go-ipfs/v0.4.14/go-ipfs_v0.4.14_linux-amd64.tar.gz"
        $ tar -xf go-ipfs_v0.4.14_linux-amd64.tar.gz
        $ sudo docker cp go-ipfs/ipfs SHELL_CONTAINER:/var/opt/ouinet

 2. Stop the injector.
 3. Run a temporary Debian container with access to the data volume:

        $ sudo docker run --rm -it -v ouinet-injector_data:/mnt debian

 4. In the container, run:

        # cd /mnt
        # rm injector/ipfs/ipfs_cache_db.*
        # alias ipfs='./ipfs -Lc injector/ipfs'
        # ipfs pin ls --type recursive | cut -d' ' -f1 | xargs ipfs pin rm
        # ipfs repo gc
        # exit

 5. Start the injector.

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

Finally start the injector.  For the local build you will need to explicitly
point it to the repository created above:

    $ <BUILD DIR>/injector --repo /path/to/injector-repo
    Swarm listening on /ip4/127.0.0.1/tcp/4001
    Swarm listening on /ip4/192.168.0.136/tcp/4001
    Swarm listening on /ip6/::1/tcp/4001
    IPNS DB: <DB IPNS>
    ...

Note down the `<DB IPNS>` string in the above output since clients will need
that as the *distributed cache index*.  You may also find this value in the
`cache-ipns` file in the injector repository.

When you are done testing the Ouinet injector, you may shut it down by hitting
Ctrl+C.

### Running a test client

To perform some tests using a Ouinet client and an existing injector, you
first need to know the *injector endpoint* and *credentials* (if needed), and
a *distributed cache index*.  These use to be respectively an IP address and
port (for testing, otherwise an I2P peer identity), a `<USER>:<PASSWORD>`
string, and an IPNS ID.

You need to configure the Ouinet client to use the aforementioned parameters.
If you have a local build, create a copy of the `repos/client` repository
template directory included in Ouinet's source tree:

    $ cp -r <SOURCE DIR>/repos/client /path/to/client-repo

When using a Docker-based client as described above, just run and stop it so
that it creates a default configuration for you.

Now edit `ouinet-client.conf` in the client repository (for Docker, use the
shell container to edit `client/ouinet-client.conf`) and add options for the
injector endpoint and credentials and the distributed cache name.  Remember to
replace the values with your own:

    injector-ep = 127.0.0.1:7070
    injector-credentials = injector_user:injector_password
    injector-ipns = Qm0123456789abcdefghijklmnopqrstuvwxyzABCDEFGI

All the steps above only need to be done once.

Finally start the client.  For the local build you will need to explicitly
point it to the repository created above:

    $ <BUIDL DIR>/client --repo /path/to/client-repo

The client opens a web proxy on local port 8080 by default (see option
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
(listening on `localhost:8080` here) by executing the following commands on
another shell:

    mkdir -p ff-profile
    env http_proxy='http://localhost:8080/' firefox --no-remote --profile ff-profile

Otherwise you may manually [modify your browser's settings][Firefox proxy] to:

  - Make the client (listening on port `localhost:8080` here) its HTTP proxy
  - Make sure that `localhost` is not listed in the *No Proxy for* field
  - Check *Use this proxy for all protocols* (mostly for HTTPS)

[Firefox proxy]: http://www.wikihow.com/Enter-Proxy-Settings-in-Firefox
    "How to Enter Proxy Settings in Firefox"

Once done, you can visit `localhost` in your browser and it should show you
the *client front-end* with assorted information from the client and
configuration tools:

  - To be able to browse HTTPS sites, you must first install the
    *client-specific CA certificate* linked from the top of the front-end page
    and authorize it to identify web sites.  The Ouinet client acts as a *man
    in the middle* to enable it to process HTTPS requests, but it (or a
    trusted injector when appropriate) still performs all standard certificate
    validations.  This CA certificate is unique to your device.

  - Several buttons near the top of the page look something like this:

        Injector proxy: enabled [ disable ]

    They allow you to enable or disable different *request mechanisms* to
    retrieve content:

      - *Origin*: The client contacts the origin server directly via HTTP(S).
      - *Proxy*: The client contacts the origin server through an HTTP proxy
        (currently the configured injector).
      - *Injector*: The client asks the injector to fetch the content from the
        origin server and inject it into the distributed cache.
      - *Cache*: The client attempts to retrieve the content from the
        distributed cache.

    Content retrieved via the Origin and Proxy mechanisms is considered
    *private and not seeded* to the distributed cache.  Content retrieved via
    the Injector and Cache mechanisms is considered *public and seeded* to the
    distributed cache.

    These mechanisms are attempted in order according to a (currently
    hard-wired, customizable in the future) *request router configuration*.
    For instance, if one points the browser to a web page which it is not yet
    in the distributed cache, then the client shall forward the request to the
    injector.  On success, the injector will (A) send the content back to the
    client and (B) seed the content to the cache.  The client will also seed
    the content (along with parts of the cache index).

  - The last seen version of the published *cache index* (an IPFS hash) is
    also shown.  It is likely that at first it shall be empty, which indicates
    that none has been downloaded yet.  The download may take from a couple of
    seconds up to about three minutes (e.g. when starting the client).  The
    page refreshes itself regularly so once the client downloads the index, it
    should display automatically.

After visiting a page with the Injector mechanism enabled and waiting for a
new index to be published and downloaded by the client (the same one or a
different one), you should be able to disable all request mechanisms except
for the Cache, clear the browser's cached data, point the browser back to the
same page and still get its contents from the distributed cache even when the
origin server is completely unreachable.

## Android library and demo client

Ouinet can also be built as an Android Archive library (AAR) to use in your
Android addps.  As a demonstration of how to integrate Ouinet in your Android
apps, Ouinet's sources include a minimal Android web browser based on the
standard `WebView` using Ouinet as a transport.

### Build requirements

A lot of free space (something less than 15 GiB).  Everything else shall be
downloaded by the `build-android.sh` script.

The instructions below use Vagrant for bulding, but the `build-android.sh`
script should work on any reasonably up-to-date Debian based system.

In the following instructions, we will use `<ANDROID>` to represent the
absolute path to your build directory.  That is, the directory from which you
will run the `build-android.sh` script (e.g. `~/ouinet.android.build`).

### Building

The following instructions will by build a Ouinet AAR library and demo client
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
Ouinet AAR library at `<ANDROID BUILD DIR>/outputs/aar/ouinet-debug.aar`
(where `<ANDROID BUILD DIR>=build-android/builddir/ouinet/build-android`
unless instructed otherwise, e.g. via Android Studio).

The script will also print out a path to the demo client APK package
(`<ANDROID BUILD DIR>/outputs/apk/debug/browser-debug.apk` by default) which
can now be copied to the host and deployed to the phone, for instance:

    vagrant $ cp /path/to/browser-debug.apk /vagrant-rw
    vagrant $ exit
    host    $ adb install /path/to/browser-debug.apk

Please note that merging different ABI builds at the same build directory is
not yet supported.  If you want to rebuild for a different ABI, you can remove
potentially conflicting files (including the generated AAR and APK!)  while
keeping downloads and ABI-neutral source files so that you can reuse them for
the next build.  Just run:

    vagrant $ /path/to/build-android.sh abiclean

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

### Setting up the demo client

The Android app has user menus for specifying the injector's endpoint,
injector's credentials, and the cache index IPNS ID, but that is very tedious
to do.  Because of that, the app also gives you an option to read QR codes.
To generate one, create a text with these values:

    injector=<INJECTOR ENDPOINT>
    ipns=<CACHE INDEX IPNS ID>
    credentials=<USER>:<PASSWORD>

and the use an online QR code generator such as [this one](https://www.the-qrcode-generator.com/),
or use a command line tool such as [qrencode](https://fukuchi.org/works/qrencode/)

    $ qrencode -t PNG -o FILE.png < FILE.txt

### Testing with Android emulator

You may also use the `build-android.sh` script to fire up an Android emulator
session with a compatible system image; just run:

    host $ /path/to/build-android.sh emu

It will download the necessary files to the current directory (or reuse files
downloaded by the build process, if available) and start the emulator.  Please
note that downloading the system image may take a few minutes, and booting the
emulator for the first time may take more than 10 minutes.  In subsequent
runs, the emulator will just recover the snapshot saved on last quit, which is
way faster.

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

First you need to compile the Ouinet library for the ABI environment your are
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

    private Ouinet _ouinet;

And in its `OnCreate` method initiate the Ouinet object:

    _ouinet = new Ouinet(this);

    _ouinet.setInjectorEndpoint("<INJECTOR ENDPOINT>");
    _ouinet.setIPNS("<CACHE INDEX IPNS ID>");
    _ouinet.setCredentialsFor("<INJECTOR ENDPOINT>", "<USER>:<PASSWORD>");

Where `<INJECTOR ENDPOINT>` may be an I2P peer identity or an `<IP>:<PORT>`
pair.  From now on all of the app's HTTP communication will be handled by
Ouinet.

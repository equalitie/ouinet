[![CircleCI](https://circleci.com/gh/equalitie/ouinet/tree/master.svg?style=shield)](https://circleci.com/gh/equalitie/ouinet/tree/master)

# Ouinet

Ouinet is a peer-to-peer, distributed technology which allows web requests to
be served with the help of an entire network of cooperating nodes using
peer-to-peer routing and distributed caching of responses.  This helps
mitigate the Web's characteristic single point of failure due to a client app
not being able to connect to a particular server.

The typical Ouinet client setup consists of a web browser (or other HTTP
client) which uses a special HTTP proxy running on the local machine and
provided by the Ouinet `client` program.  Web requests performed by the
browser are sent to the local Ouinet `client`, which attempts to retrieve the
resource using several mechanisms.  The client tries to fetch the page from a
distributed cache (like [IPFS][]), and if not available, it contacts a trusted
*injector* server over a peer-to-peer routing system (like [GNUnet][] or
[I2P][]) and asks it to fetch the page and store it in the distributed cache
to speed up future accesses.

[IPFS]: https://ipfs.io/ "InterPlanetary File System"
[GNUnet]: https://gnunet.org/ "GNU's Framework for Secure Peer-to-Peer Networking"
[I2P]: https://geti2p.net/ "Invisible Internet Project"

**Warning:** Ouinet is still highly experimental.  Some features (like
peer-to-peer routing) may or may not not work smoothly depending on the
different back end technologies.  Information about your browsing might be
leaked to other participants in the network.  Running some components (like
injector code) may turn your computer into an open web proxy, and other
security or privacy-affecting issues might exist.  Please keep this in mind
when using this software.

**Note:** The steps described below have only been tested to work on GNU/Linux
on AMD64 platforms.

## Using the easy installation script

The [scripts/build-ouinet.sh][build-ouinet.sh] script can be used to download
all necessary source code, build the Ouinet library and tools and install
them, all with a simple command invocation.  If you do not already have it in
your computer, just download it and copy it to some temporary directory.  Then
open a shell in that directory and run:

    sh build-ouinet.sh

[build-ouinet.sh]: https://raw.githubusercontent.com/equalitie/ouinet/master/scripts/build-ouinet.sh

The script will first check that you have all the needed system packages.  If
you do not, it will show an error like:

    Missing dependencies:  some-package some-other-package
    Ignore this warning with --force.

The names of missing dependencies correspond to package names in a Debian or
Ubuntu-based system.  To install them, just run:

    sudo apt update
    sudo apt install some-package some-other-package

In other platforms the names of packages may differ and you may need to figure
them out and install them manually.

After installing missing packages you can run the script again:

    sh build-ouinet.sh

You may need to repeat this until the script succeeds and reports instructions
on how to run the client or injector tools.  The whole process takes a few
minutes and requires around 2 GB of storage.

### Testing

To perform some tests using the just-built Ouinet client and an existing
injector, you first need to know the *injector endpoint* and a *distributed
cache name*.  These use to be an IP address and PORT, and an IPNS identifier,
respectively (although the injector endpoint may also be a GNUnet peer
identity and service name).

You need to configure the Ouinet client to use the aforementioned parameters.
Edit the included configuration file:

    edit ouinet-repos/client/ouinet-client.conf

Add options there for the injector endpoint and the distributed cache name.
Remember to replace the values with your own:

    injector-ep = 192.0.2.1:1234
    injector-ipns = Qm0123456789abcdefghijklmnopqrstuvwxyzABCDEFGI

All the steps above only need to be done once.

Before you start the Ouinet client, you must run some local GNUnet services.
Execute the following command:

    env BUILD=ouinet-build REPOS=ouinet-repos ouinet/scripts/start-gnunet-services.sh client & gn=$!

Give it a few seconds and start the Ouinet client by running:

    ouinet-build/client --repo ouinet-repos/client

The client opens a web proxy on local port 7070 (see option `listen-on-tcp` in
its configuration file).  If you have Firefox installed, you can create a new
profile (stored under the `ff-profile` directory in the example below) which
uses the Ouinet client as a proxy by executing the following commands on
another shell:

    mkdir -p ff-profile
    env http_proxy='http://localhost:7070/' firefox --no-remote --profile ff-profile

When you access the web using this browser, your requests will go through your
local Ouinet client, which will attempt several mechanisms supported by Ouinet
to retrieve the resource.

When you are done testing the Ouinet client, just shut down the browser, the
client itself (by hitting Ctrl+C) and finally the GNUnet services by running:

    kill $gn

## Requirements

* `cmake` 3.5+
* `g++` capable of C++14
* The [Boost library](http://www.boost.org/)

Note: The Go language and the IPFS project will be downloaded automatically
during the build process.

## Clone

Ouinet uses Git submodules, thus to properly clone it, use

```
$ git clone --recursive git@github.com:equalitie/ouinet.git
```

OR

```
$ git clone git@github.com:equalitie/ouinet.git
$ cd ouinet
$ git submodule update --init --recursive
```

## Build

```
# Assuming:
#   * <PROJECT ROOT> points to the directory where the
#                    CMakeLists.txt file is
#   * <BUILD DIR> is a directory of your choice where all
#                 (even temporary) build files will go
$ mkdir -p <BUILD DIR>
$ cd <BUILD DIR>
$ cmake <PROJECT ROOT>
$ make
```

## Test

Before everything else, we need to start GNUnet services. To do that
open a new terminal window and execute:

```
$ ./scripts/start-gnunet-services.sh
```

Leave that script running and start another terminal window where we'll start
the injector:

```
$ ./injector --repo ../repos/injector
Swarm listening on /ip4/127.0.0.1/tcp/4001
Swarm listening on /ip4/192.168.0.136/tcp/4001
Swarm listening on /ip6/::1/tcp/4001
IPNS DB: <DB_IPNS>
...
GNUnet ID: <GNUNET_ID>
...
```

Make note of the `<DB_IPNS>` and `<GNUNET_ID>` strings in the above output,
we'll need to pass them as arguments to the client.

While injector is still running, start the client in yet another terminal
window and pass it the injector's `<GNUNET_ID>` and `<DB_IPNS>` strings from
above:

```
$ ./client --repo ../repos/client \
           --injector-ipns <DB_IPNS> \
           --injector-ep <GNUNET_ID>:injector-main-port
```

Now [modify the settings of your
browser](http://www.wikihow.com/Enter-Proxy-Settings-in-Firefox) to make the
client - which runs on port localhost:7070 - it's proxy. Also **make sure
'localhost' is not listed in the `"No Proxy for"` field**. Once done, you can
enter `localhost` into your browser and it should show you what database of
sites the client is currently using.

It is likely that at first the database shall be `nill` which indicates that
no database has been dowloaded from IPFS yet. This may take from a couple of
seconds up to about three minutes. The page refreshes itself regurarly so
once the client downloads the database, it should display automatically.

In the mean time, notice also the small form at the top of the page looking
something like this:

```
Injector proxy: enabled [disable]
```

This means that proxing to injector is currently `enabled`, which in turn
means that if one points the browser to a non secure http page and the page
isn't yet in the IPFS database, then the client shall forward the HTTP
request to the injector. On success, injector will (A) send the content
back and (B) upload the content to the IPFS database.

Each time the injector updates the database it prints out a message:

```
Published DB: Qm...
```

Once published, it will take some time for the client to download it
(up to three minutes from experience) and once it does so, it will be shown
on client's frontend.

At that point one can disable the proxying through injector, clear
browser's cached data and try to point the browser to the same non secured
HTTP page.

```
$ ./test.sh <BUILD DIR>/client
```


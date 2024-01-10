<img src="https://ouinet.work/img/ouinet-logo.png" width=250px alt="Ouinet">

[![pipeline status](https://gitlab.com/equalitie/ouinet/badges/main/pipeline.svg)](https://gitlab.com/equalitie/ouinet/commits/main)
[![release](https://gitlab.com/equalitie/ouinet/-/badges/release.svg)](https://gitlab.com/equalitie/ouinet/-/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-brightgreen.svg)](./LICENSE)

Ouinet is a Free/Open Source collection of software tools and support
infrastructure that provide access to web resources when access to the
unrestricted internet is unreliable or unavailable, tailored for scenarios of
limited Internet connectivity and selective network traffic filtering.


## How it works?

A typical [client][] node setup consists of a web browser or other application
using the special HTTP proxy provided by Ouinet. When the Ouinet proxy gets a
request for content, it attempts to retrieve the resource using several
mechanisms. For example, it could try to fetch a page from the [distributed cache][]
by looking up the content in the BitTorrent DHT and if the content is not
available, it could also contact a trusted [injector][] server over a
peer-to-peer routing system (like the [BitTorrent DHT][] or [I2P][]) and ask to
fetch the page and store it in the distributed cache.

See our [lightning talk at the Decentralized Web Summit 2018][] for an
overview of Ouinet's architecture or check the [documentation website][].

For a detailed technical explanation of processes and protocols you can refer
to [Ouinet's white paper][].

[lightning talk at the Decentralized Web Summit 2018]: http://archive.org/details/dweb-8_2_18_Lightning_Talks_New_Discoveries_5?start=547
[documentation website]: https://ouinet.work/docs/how/index.html
[Ouinet's white paper]: doc/ouinet-network-whitepaper.md
[BitTorrent DHT]: http://bittorrent.org/beps/bep_0005.html
[I2P]: https://geti2p.net/ "Invisible Internet Project"
[client]: https://ouinet.work/docs/how/client.html
[injector]: https://ouinet.work/docs/how/injectors.html
[distributed cache]: https://ouinet.work/docs/how/cache.html

![Ouinet request/response flow](./doc/diagrams/simple-request-flow.svg)

**Warning:** Ouinet is **not an anonymity tool**, information about your
browsing might be seen by other participants in the network, as well as the
fact that your application is seeding particular content.  Running some
components (like injector code) may turn your computer into an open web proxy,
and other security or privacy-affecting issues might exist.  Please keep this
in mind when using this software and only assume reasonable risks.


## Request mechanisms to retrieve content

These mechanisms are attempted as parallel coroutines showing the results of
the first responding method.

- *Origin*: The client contacts the origin server directly via HTTP(S).
- *Proxy*: The client contacts the origin server through an HTTP proxy
  (currently the configured injector) and retrieves the content without
  signing it.
- *Injector*: The client asks the injector to fetch and sign the content
  from the origin server, then it starts seeding the signed content to
  the distributed cache.
- *Distributed Cache*: The client attempts to retrieve the content from
  the distributed cache.


## Software Artifacts

* **Client**: Command line application that serves as a proxy to the Ouinet
network.
* **Injector**: Command line application that retrieves content from origin
websites and injects the signed content to the Ouinet network so it can be
shared peer-to-peer.
* **Android libraries**: Java Native Interface used to expose the C++ Ouinet
networking libraries to the Android applications written in Java or Kotlin.


## Building from source

The following instructions were tested in Debian 12 with the following
packages installed; `build-essential`, `cmake`, `git`, `libssl-dev` and
`zlib1g-dev`, but in general to build Ouinet natively on your GNU/Linux
system you just need *CMake 3.5+* and *g++* capable of C++14.

Ouinet uses Git submodules, thus to properly clone it, use:

    $ git clone --recursive https://gitlab.com/equalitie/ouinet.git

Assuming that `<SOURCE DIR>` points to the directory where the
`CMakeLists.txt` file is, and `<BUILD DIR>` is a directory of your choice
where all (even temporary) build files will go, you can build Ouinet with:

    $ mkdir -p <BUILD DIR>
    $ cd <BUILD DIR>
    $ cmake <SOURCE DIR>
    $ cmake --build <BUILD DIR>

When the build process finishes you will find in `<BUILD DIR>` the binaries
for `client`, `injector` and their shared libraries, e.g. `libboost_asio.so`,
`libcpp_upnp.a`, etc.


## Running the Injector

It's recommended to create a directory named `repos/injector` where the
configuration file, the ed25519 keys and the TLS certificates will be stored.

The minimum configuration to start the Injector is shown below:

    # repos/injector/ouinet-injector.conf

    listen-on-utp-tls = 0.0.0.0:7085
    credentials = test_user_change_me:test_password_change_me

You could also find more details of the available options in this
[config example](repos/injector/ouinet-injector.conf) or invoking
`injector --help`.

When your `repo` dir and the configuration file are ready you can start
the injector as follows:

    $ ./injector --repo /path/to/your/repo

During its first start the injector will generate the private and public keys
needed to [sign content](https://ouinet.work/docs/how/cache.html#signatures)
(`ed25519-*` files) and the certificates used to establish a TLS connection
when contacting the injector via uTP protocol. Please keep an eye on these
files as some of them will be needed to configure your Ouinet clients.


## Running a Client

Create a repo directory, e.g. `repos/client` and add the configuration file
for the new client:

    # repos/client/ouinet-client.conf

    cache-type = bep5-http
    cache-http-public-key = abcdefghijklmnopqrstuvwxyz01234567890abcdefghijklmno
    injector-tls-cert-file = /path/to/your/repo/client/tls-cert.pem
    injector-credentials = test_user_change_me:test_password_change_me

The value of `cache-http-public-key` can be obtained from the injector file
named `ed25519-public-key` or from the injector log entry that starts with
`[INFO] Injector swarm: sha1('ed25519:abcdefghijklmnopqrstuvwxyz01234567890abcdefghijklmno/v6/injectors`.

`injector-tls-cert-file` is the path to the `tls-cert.pem` copied from the
injector and `injector-credentials` should be set to the same value defined
as `credentials` in `ouinet-injector.conf`.

When the config file is ready you can start the client as follows:

    $ ./client --repo /path/to/your/repo

For more details about configuration options please run `./client --help`.

Once done, you can visit `localhost:8078` in your browser and it should show
you the client front-end with assorted information from the client and
configuration tools. The client's HTTP proxy endpoint should be available to
the host at `localhost:8077`.


## Testing the Client with cURL

Now that the Ouinet services are running we will test a simple scenario where
a Client requests a URL from the Injector.

It's important to disable `Origin` and `Proxy` mechanisms to force Ouinet to
fetch the content from the Injector. To do this, you can press the `disable`
button in Ouinet's front-end (that's running by default at `localhost:8078`)
or to set the values `disable-origin-access` and `disable-proxy-access` to
`true` in Ouinet's config and restart the service.

The following example requests `https://ouinet.work` from Ouinet's proxy
running on port `8077` and receives an HTTP response with `x-ouinet-source`
header set to `["injector"]`. SSL verification is skipped with `--insecure`
just to keep the example as simple as possible but for production applications
the Ouinet TLS certificate should be installed and validated.


    $ curl https://ouinet.work \
        --header 'X-Ouinet-Group: ouinet.work' \
        --proxy 127.0.0.1:8077 \
        --insecure \
        --silent \
        --output /dev/null \
        --write-out '%{http_code},%{header_json}' | \
      grep x-ouinet-source

    "x-ouinet-source":["injector"]

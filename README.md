# Ouinet

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

### Important setup step if you're running the injector and the client on the same PC

If you're running both applications on the same PC you need to set up
one of them to use different port. In what follows, we'll do this change
on the injector.

First we need to create the default config file by running the injector
and then closing it soon after:

```
$ ./injector 0.0.0.0 8080 # Then Ctrl-C after ~10 seconds
```

Then open injector's config file `injector_repo/config` and modify the part

```
  "Addresses": {
    "Swarm": [
      "/ip4/0.0.0.0/tcp/4001",
      "/ip6/::/tcp/4001"
    ],
```

By replacing the port `4001` with `0` as so:

```
  "Addresses": {
    "Swarm": [
      "/ip4/0.0.0.0/tcp/0",
      "/ip6/::/tcp/0"
    ],
```

This will ensure that the injector will always start to listen on a random
available port.

### Browser

Start the injector and make note of the `<DB_IPNS>` string in the output:

```
$ ./injector 0.0.0.0 8080
Swarm listening on /ip4/127.0.0.1/tcp/4001
Swarm listening on /ip4/192.168.0.136/tcp/4001
Swarm listening on /ip6/::1/tcp/4001
IPNS DB: <DB_IPNS>
```

Now - while injector is still running - start the client in another terminal
and pass it the injector's address (`192.168.0.136` in the example above,
`127.0.0.1` if the client will run on the same PC as the injector) and the
`<DB_IPNS>` string from above:

```
$ ./client 0.0.0.0:7070 <INJECTOR_ADDRESS>:8080 <DB_IPNS>
```

Now [modify the settings of your
browser](http://www.wikihow.com/Enter-Proxy-Settings-in-Firefox) to make the
client its proxy, and make sure 'localhost' is not listed in the "No Proxy for"
field. Once done, you can enter `localhost:7070` into your browser and it
should show you what database of sites the client is currently using.

It is likely that at first the database shall be `nill` which indicates that
no database has been dowloaded from IPFS yet. This may take from a couple of
seconds up to about three minutes. The page refreshes itself regurarly so
one the client downloads the database, it should display automatically.

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

### Unit

Currently only test to see if forwarding to injector works

```
$ ./test.sh <BUILD DIR>/client
```


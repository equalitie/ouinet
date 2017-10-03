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

### Browser

Start the injector and make note of the `<DB_IPNS>` string in the output:

```
$ ./injector 0.0.0.0 8080
Swarm listening on /ip4/127.0.0.1/tcp/4001
Swarm listening on /ip4/192.168.0.136/tcp/4001
Swarm listening on /ip6/::1/tcp/4001
IPNS DB: <DB_IPNS>
```

Now - while injector is still running - start the client and pass
it the `<DB_IPNS>` string from above:

```
$ ./client 0.0.0.0 7070 <DB_IPNS>
```

At this point, the ipfs-cache should be empty. You can test this by [setting up
your browser proxy](http://www.wikihow.com/Enter-Proxy-Settings-in-Firefox) to
point to `localhost:7070` (i.e. it will try to get the data through the client)
and it should respond with an error message.

To inject content into the ipfs-cache, set up your browser to point to the
injector (i.e. `localhost:8080`) and type in some **non secure** HTTP URL into
it. While the page is being rendered into the browser, the injector is also
pushing it into the ipfs-cache. Note that a page appearing on the screen
doesn't necessarily mean it's already in the cache.  That might take from a few
seconds to up to a ~2 minutes.

To see the cache content, set your proxy to point to the client again
(`localhost:7070`) and then try to open the same URL as you did in the above
paragraph.

### Unit

Currently only test to see if forwarding to injector works

```
$ ./test.sh <BUILD DIR>/client
```


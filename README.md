# Ouinet

## Requirements

* `cmake` 3.5+
* `g++` capable of C++14
* The [Boost library](http://www.boost.org/)
* IPFS
* The Go language

## Clone

Ouinet uses git submodules, thus to properly clone it, use

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

### Get Go and build IPFS

To install the Go language and go-ipfs, set up your GOPATH environment
variable to point to a directory where all your `go-ipfs` source files will go.
E.g.

```
$ export GOPATH=$HOME/go
```

And THEN follow [these instructions](https://github.com/ipfs/go-ipfs#build-from-source).

### Build ouinet

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

```
$ <PROJECT ROOT>/test.sh <BUILD DIR>/client
```


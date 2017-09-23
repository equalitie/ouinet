# Ouinet

## Requirements

* `cmake` 3.5+
* `g++` capable of C++14
* The [Boost library](http://www.boost.org/)

## Building

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

## Testing

```
$ <PROJECT ROOT>/test.sh <BUILD DIR>/ouinet
```


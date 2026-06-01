# The `find_package` can use 'Config mode' or 'Module mode' [0]. If the
# configuration reached into this file, it means it's using the 'Module mode'.
# But we want it to use the 'Config mode' so we delegate and the
# BoostConfig.cmake will be used instead.
#
# [0] https://cmake.org/cmake/help/latest/guide/using-dependencies/index.html#guide:Using%20Dependencies%20Guide

find_package(Boost CONFIG)

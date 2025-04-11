# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).


## Unreleased

### Changed

- The default Boost version used by Ouinet is now 1.87.0.
- Docker builder images for Android and Linux have now installed a Rust toolchain.


## [v0.31.1](https://gitlab.com/equalitie/ouinet/-/releases/v0.31.1) - 2025-01-28

### Changed

- Functionality to `OuinetBackground` kotlin wrapper was modified, allowing the
notification associated with the `OuinetService` to be disabled.

### Fixed

- URL to download Boost is pointing now to `archives.boost.io`.


## [v0.31.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.31.0) - 2024-11-13

### Changed

- Ouinet binaries and its dependencies are built now with C++ 20.
- Renamed and refactored `debian-12` jobs and images to have a single
  `linux` CI pipeline.
- Minor changes and improvements to the CI tests and artifacts rules.

### Fixed

- Fix for connectivity state monitor, restart ouinet even when no networks
  are available.
- Added a timer to wait for the DHT to be ready in bittorrent tests.

### Removed

- The `test_bep_44` was disabled because it was failing randomly in the CI
and the feature that covers is not in use.


## [v0.30.1](https://gitlab.com/equalitie/ouinet/-/releases/v0.30.1) - 2024-10-29

### Fixed

- A `_cancel` signal is now triggered when a DHT instance is stopped, instead
of waiting until its destructor is invoked. This prevents the spawning of
coroutines when the client process is stopped and also releases pending
locks on the shared pointer to the DHT instance, reducing the time needed to
perform a clean shutdown.


## [v0.30.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.30.0) - 2024-10-14

### Added

- New option `udp-mux-port` to control the port used by Ouinet's UDP multiplexer.

### Fixed

- Added a delay between the attempts of the UPnP updater to get a local
  address. This is to prevent the loop from filling the logs when a network
  adapter is not present, e.g. when the device is in airplane-mode.

### Changed

- Avoid blocking the Ouinet service initialization by running the DHT
  bootstrapping in a coroutine.
- Ouinet client state is now set to `degraded` when the proxy is ready
  to accept requests but the DHT is not bootstrapped.
- Full refactoring of `create_udp_multiplexer.h`.
- Changed the order of the port binding attempts to `settings`, `random`,
  `last_used`, `default` and `last_resort`.

### Deprecated

- Setting the `udp-mux-port` option is now preferable to use the file
  `last_used_udp_port`. The possibility of using this file to set the UDP
  port will be removed in future releases.


## [v0.29.1](https://gitlab.com/equalitie/ouinet/-/releases/v0.29.1) - 2024-09-20

### Changed

- Updated list of bootstrap servers.

### Removed

- Disabled bencoding validation requiring sorted keys in dictionaries of
the KRPC messages sent to the DHT.


## [v0.29.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.29.0) - 2024-08-21

### Changed

- Applied adjustments that are required to build the `windows-client`
branch but that are not exclusively affecting to Windows builds.
- Upgrade `nlohmann/json` package to version 3.11.3

### Removed

- CI pipelines that were building Ouinet with Debian 12 and Boost 1.77.0.


## [v0.28.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.28.0) - 2024-08-07

### Added

- A new `file_io` component that supports asynchronous I/O operations for
Windows systems.


## [v0.27.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.27.0) - 2024-07-08

### Added

- New CMake option `WITH_EXPERIMENTAL` set to `OFF` by the default that avoids
compiling unused features, including i2p, pluggable transports like obfs4.
- Configurable error page feature that allows path to html file to be pass
into Ouinet config for android applications. The supplied html file is copied
into the Ouinet client's assets and served in place of plain text failure
message.

### Removed

- Commands in Dockerfile related to the installation of i2p and the pluggable
transports.
- Configuration options in client and injector related to i2p and obfs
endpoints.
- Deprecated CI pipelines that were building Ouinet with Debian 10.
- Deprecated CI pipelines building Ouinet with Debian 12 and Boost 1.71-1.74.


## [v0.26.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.26.0) - 2024-06-26

### Changed

- The default Boost version used by Ouinet is now 1.79.0.
- References to `ifstream` and `ofstream` of `boost::filesystem` were replaced
with `boost::nowide::fstream`.

### Deprecated

- Configured Debian 12 pipelines using Boost 1.77 to run only when are
manually triggered.


## [v0.25.1](https://gitlab.com/equalitie/ouinet/-/releases/v0.25.1) - 2024-05-30

### Fixed

- Crashes in Android 9 and 10 devices that were caused by `boost::filesystem`
when attempting to use `statx` instructions.
- Dependencies of the CI jobs were changed as they were still using names
related to the deprecated Debian 10 pipelines.

### Deprecated

- Configured Debian 12 pipelines using Boost 1.71 and Boost 1.74 to run only
when are manually triggered.
- All the Debian 10 pipelines were also marked only for manual execution.


## [v0.25.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.25.0) - 2024-05-15

### Changed

- The default Boost version used by Ouinet is now 1.77.0.
- All the Docker images including Ouinet builders and production clients
will use Debian 12 as its base operating system.
- Targets depending on libssl libraries are now using OpenSSL v3.
- The build environment uses now Gradle 8.7, AGP 8.3 and Kotlin 1.9.23.
- Unit tests dependencies of the JNI implementation for Android were
refactored to use JUnit 5, Mockito 4.3.

### Fixed

- Issues when installing `git.torproject.org/pluggable-transports/goptlib`
were fixed by upgrading Go to version 1.22.

### Removed

- PowerMock is not used anymore by Android tests.

### Deprecated

- Support for Ouinet builds using Boost 1.71.0 and 1.74.0 are now deprecated
and its CI pipelines will be removed in the next release.
- Use of Docker images with Debian 10 will be also removed in the next release.


## [v0.24.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.24.0) - 2024-04-09

### Added

- Plan for Ouinet [programming language interoperability](doc/arch-drafts/programming-language-interoperability.md).
- New test suite to verify the `util/file_io` component in Windows and Linux
platforms.
- Added test cases in `test/test_atomic_temp` to cover `util/atomic_file` and
`util/temp_file`.

### Changed

- README file revamping, including the update of code examples, references and
diagrams.
- Sets the target SDK for the Ouinet AAR build to API 34.

### Fixed

- Fix include directives in `src/cache` and `ouiservice/multi_utp_server` that
were using relative routes.
- Fix `OuinetService` foreground notification to work correctly on Android 14.
    - Add `RECEIVER_NOT_EXPORTED` flag when registering notification receiver.
    - Set package name on intent for notification receiver.


## [v0.23.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.23.0) - 2023-11-24

### Added

- New CLI option named `disable-bridge-announcement` that avoids BEP5
announcements of the client address as a Bridge/Helper to the DHT.

### Changed

- Refactoring of Ouinet sources and its submodules to be able to build them with
Boost 1.77.


## [v0.22.0](https://gitlab.com/equalitie/ouinet/-/releases/v0.22.0) - 2023-10-12

### Added

- New jobs to the CI to build and test Ouinet with different
versions of Boost.

### Changed

- Refactored Ouinet sources and its submodules to be able to build them with
Boost 1.74.
- Replaced the direct usage of `asio::executor` with a custom type named
`AsioExecutor` that loads accordingly to the Boost version used.
- Updated the User Agent reported by Ouinet to avoid issues with
websites that limit functionality based on this header.


## [v0.21.11](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.11) - 2023-08-30

### Added

- Created separate Docker images for the different target platforms.

### Changed

- Patched CMake scripts to make them compatible with Debian 12.
- Patched submodules `asio-utp`, `cpp-upnp` and `i2pd` to make them
compatible with newer versions of GCC and CMake.
- Patched the Thread library of Boost 1.71 to compile it with Debian 12.
- Refactoring of `.ci-gitlab.yml` into separate job and pipeline files,
defined rules and dependencies for Android and Linux jobs and added a
pipeline to build Ouinet using Debian 12.
- Updated the Kotlin version used in Gradle scripts.

### Fixed

- Fix a segmentation fault when decoding a BencodedValue.


## [v0.21.10](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.10) - 2023-04-13

### Changed

- Update to `ConnectivityStateMonitor` which implements the NetworkCallback class
as a replacement for the `ConnectivityBroadcastReceiver`.
- Use the `ConnectivityStateMonitor` to more accurately track when existing
connections are lost and new connections are available.
- Remove unused `broadcastReceivers` to avoid possible conflicts/confusion with
receivers and monitors implemented in new kotlin portion of code.

### Fixed

- Fixes a bug with the Ouinet notification not appearing upon startup, reported
in censorship-no/ceno-browser#53 that was caused by an attempt to restart ouinet
being triggered too soon after startup due to a perceived change in network
connectivity.
- Avoid attempting to restart ouinet on the first network availability, or
before ouinet startup is complete, or after a 10 second timeout after initial
ouinet startup request.


## [v0.21.9](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.9) 2023-03-28

### Changed

- Introduce callbacks to start and stop threads.
- Allow consuming application to join the `start` or `stop` threads by returning
the thread when calling the `start`, `stop`, `startup`, and `shutdown` methods.
- Remove dependency on activity in `OuinetBackground`.

### Fixed

- Only send pending intents to notification when Ouinet client state changes.
- Call `getState()` on Ouinet object instead of just `state`, which appeared to
be crashing the client.


## [v0.21.8](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.8) 2023-03-07

### Added

- Add new `max-simultaneous-announcements` option, which defines the number
of simultaneous BEP5 announcements to the DHT. Default for Android
devices is 1 and for the desktop ones is 16.

### Changed

- Spawning batches of announcement as co-routines instead of processing them
sequentially.


## [v0.21.7](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.7) 2023-03-06

### Added

- OuinetBackground feature now available which allows for a Ouinet client to be
managed more effectively by the application that implements Ouinet using a
background service and a foreground notification that keeps the process alive.
- NotificationConfig added for customizing notification UI.

### Changed

- Gradle build now retreives branch and commit ID without shell script.
- Adds gradle.properties to assit in building new kotlin code.

### Removed

- Support of Android SDK for armeabi-v7a and x86 API older than 21


## [v0.21.6](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.6) 2023-02-17

### Fixed

- Fix needed for releasing v0.21.6 ouinet android library. This fixes the
signing of AARs when publishing to Sonatype since gradle 7 changed where
task names are found and became more opinionated on how to find sub-strings
in an array of strings, instead match full task name.


## [v0.21.5](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.5) 2023-01-04

### Fixed

- Reverts changes made in v0.21.3 and v0.21.4 and applies a new fix for a
crash in the Android library when the stop methods are invoked from a
non-stopped state or when the Ouinet service is switching between WiFi and
mobile connections.


## v0.21.4 2023-01-03

### Fixed

- Fixes a memory leak in the Android library when
the Ouinet service is restarted by changes in network connectivity,
e.g. switching between WiFi and mobile connections.
- Removes unused broadcast recievers that caused memory leak
if they were not unregistered successfully.


# Appendix A: Git tags reference

This section lists older tags in the repository as a reference to the changes
released before adhering to the [Keep a Changelog](https://keepachangelog.com/)
spec. The tags description will be used to gradually import previous releases
to the changelog format.


## v0.21.3 (2022-10-12)

Release v0.21.3.
This minor release fixes a crash in the Android library when the stop
methods are invoked from a non-stopped state or when the Ouinet service
is switching between WiFi and mobile connections.
Bug fixes:
- Fix the release of WifiManager multicast locks when Ouinet is
stopped.
- Fix errors related to subsequent calls to Ouinet stop methods.
- ouinet v0.21.3

## v0.21.2 (2022-07-13)

Release v0.21.2.
This release updates the OpenSSL library in Android to a recent version (not
that we found any bugs affecting Ouinet itself), and it provides a new `omni`
AAR compatible with all Android ABIs supported by Ouinet.  It also includes
many fixes in the handling of cancellation errors, making logs more reliable
and informative, besides avoiding unwanted cancellations in some corner cases.
Features:
- Support in build scripts for a new `omni` pseudo-ABI for Android which
produces a single AAR for ARM32, ARM64 and x64 ABIs.  Useful for creating
Android app bundles (AAB packages).
Enhancements:
- The OpenSSL build dependency in Android has been updated to version 1.1.1q.
- If in verbose mode, the client now logs when a group is newly announced, or
stopped being announced.
Bug fixes:
- Fix some functions triggering a cancel object passed by reference by the
caller.
- Fix many error returns not checking for cancellation.
- Fix some cancellation errors being reported where a timeout was expected.
- Fix some cancellation errors in the generic stream implementation being
reported when a "shut down" error was expected.
- Do not override some underlying errors in multi-peer reader with a
cancellation error.
- ouinet v0.21.2

## v0.21.1 (2022-06-15)

Release v0.21.1.
This minor release fixes a crash at the injector when an error occured on
incoming request read, and avoids similar future crashes.
Enhancements:
- Report when the connection serve operation at the injector leaks an
error (instead of crashing).
Bug fixes:
- Revert to not leaking an error when reading a request at the injector fails.
This made the injector crash.
- Revert to not leaking an error when the TLS handshake operation fails.  This
was innocuous but logged an excessively alarming error message.
- ouinet v0.21.1

## v0.21.0 (2022-06-14)

Release v0.21.0.
This release adds options to use custom extra servers to bootstrap the
BitTorrent DHT (in case that the embedded ones are not usable for some
reason), available as the `bt-bootstrap-extra` option and via the client
front-end.  It also enhances the front-end's input controls by using proper
labels and extra shortcuts, and makes them addressable via URL fragments,
highlighting them on page load.  Finally, many fixes for timeout handling (in
response body flushing and elsewhere) are included.
Features:
- New `bt-bootstrap-extra` option (for client and injector) to add several
extra BitTorrent bootstrap servers.  The option is persistent at the client
and available in the front-end and status API (as `bt_extra_bootstraps`).
When changed via the front-end, it is applied on subsequent runs.  Also
available in the Android wrapper.
- Client front-end inputs can be addressed via URL fragments (with identifier
`input-<INPUT_ID>`).  Some minimal Javascript highlights the selected input.
Enhancements:
- Properly tag as labels client front-end texts which accompany inputs.
- Add keyboard shortcuts for client front-end inputs missing them.
- Fix missing escaping of values inlined in client front-end HTML.
- Remove internal boilerplate in cancellation and timeout handling by defining
(and using!) functions and macros (and put them in the headers where they
really belong).
Bug fixes:
- Apply timeouts to response body flush operations at the client.
- Fix timeout not triggering cancellation in response part timed read
operation.
- Do check for timeout events after many async operations.
- Fix error code not getting reset in injector listen loop.
- ouinet v0.21.0

## v0.20.0 (2022-06-01)

Release v0.20.0.
The main news for this release of Ouinet is the reworking of Android library
build machinery to finally support creating signed releases and uploading them
to the Maven Central repository.  This, along with updated instructions in the
readme, should make integration of Ouinet into other apps much simpler.
Features:
- Update Gradle configuration and shell scripts to allow uploading a signed
build of the Android library to Maven Central.
Enhancements:
- Make client local proxy and front-end endpoints configurable in Android.
You should use in your app some non-default values to avoid clashing with
other Ouinet-enabled apps running on the same device.
- Update readme with new instructions for integration in Android apps.
- Report errors in client writing response to cache and user agent.
- Tag long-running operations in client fetch fresh from injector.
- Log client authentication failures at the injector.
- ouinet v0.20.0

## v0.19.0 (2022-05-23)

Release v0.19.0.
This release's main novelty is the support for client persistent options,
i.e. options whose value gets saved when changed from the command line or the
front-end.  Option parsing has received some functionality, consistency and
readability fixes.  Finally, logging of "operation not supported" errors is
more informative now.
Features:
- Handle some options as persistent & save them to `ouinet-client.saved.conf`
in the client repo directory.  All such options are saved every time one of
them is changed (either from the command line or the front-end).  This
includes `log-level`, existing `disable-*-access` options, and the new
`disable-cache-access` and `enable-log-level` options.  A new option
`drop-saved-opts` drops all saved values.
- Add new `disable-cache-access` option independent from `cache-type=none`, so
that the cache can be enabled but still not used for retrieving content.
This setting was only possible from the client front-end previously.
- Add new `enable-log-file` option to write log messages to `log.txt` in the
client repo directory.  This setting was only possible from the client
front-end previously.
Enhancements:
- Make `--help` messages more consistent and better structured.
- Make option processing errors more consistent.
- Better logging of code reporting "operation not supported".
- ouinet v0.19.0

## v0.18.2 (2022-04-12)

Release v0.18.2.
This release fixes a few issues with body size computation which prevented an
efficient use of the local cache and interacted badly with static cache
functionality.
Enhancements:
- Allow serving to the user agent entries in the local cache with incomplete
or missing body data for `HEAD` requests.
Bug fixes:
- Fix miscomputing body data size as 0 for static cache entries when the entry
is not also in the internal cache.  This made cache code regard all static
cache entries as incomplete and trigger unneeded multi-peer downloads which
may fail and render the entries in the static cache inaccessible.
- Avoid getting a reader from the internal cache and its body data size from
the static cache, when both entries exist for the same key in the HTTP store.
- ouinet v0.18.2

## v0.18.1 (2022-04-07)

Release v0.18.1.
Besides some debug logging enhancements, this release makes local access to
the cache more usable with default Ouinet settings (i.e. with all request
mechanisms on), instead of requiring to disable Origin or Injector mechanisms
to avoid stuck downloads or longer initial delays.  Also, an additional
BitTorrent bootstrap node has been included by its IP address (in case of DNS
failure).
Enhancements:
- Avoid certain problematic behaviors at the client when injector access is
still starting (which may be caused by a lack of external network
connectivity): (i) do not spawn a parallel fetch fresh when fetching stored
content, (ii) make revalidations fail fast for already stored content,
and (iii) shorten the delay to start the injector/cache retrieval job
relative to the origin job.
- Include the IP address of one of eQualitie's bootstrap nodes already pointed
by `router.bt.ouinet.work`, to be used in case of DNS failure.
- Better messages for "not ok to cache" debug log entries.  Also, they always
appear whenever an Injector response is not stored.
- ouinet v0.18.1

## v0.18.0 (2022-03-29)

Release v0.18.0.
This release allows the Ouinet client to access its local cache even when it
has no connectivity at all, and to exchange cached content with other clients
in the same LAN when it has no upstream connection to the Internet.  This
should allow for instance to browse content previously visited (and cached) in
the same device, or to share content with close devices in cases of extreme
lack of connectivity.
Also, all operations forwarding traffic for other nodes (both at the injector
and the client) report forwarded byte counts in the log, to enable computing
some bandwidth usage statistics.
Features:
- Log counts of forwarded bytes in injector signing and proxy (plain &
tunnel) (always), and in client content serving and bridge tunnel (with
debug on).
Enhancements:
- Allow the operation of the cache client before bootstrapping the BitTorrent
DHT (it can be used once bootstrapped).
- Update and simplify Guix build environment instructions.
Bug fixes:
- Fix unhandled exception in local discovery start with no connectivity.
- Other minor cancellation fixes on waiting for the start of components, in
the full-duplex forward operation and elsewhere.
- ouinet v0.18.0

## v0.17.1 (2022-03-08)

Release v0.17.1.
This minor release fixes bugs which may cause buffered data in
client-to-client and Proxy client-to-origin connections getting discarded,
thus rendering them unusable (usually manifesting as response body retrievals
getting stuck after the first block).
Enhancements:
- With debug on, report resulting error code at the end of each connection
served by injector and client.
Bug fixes:
- Avoid buffered but yet unused data from getting discarded across requests in
the same client-to-client connection (regression in v0.17.0) or
client-to-origin CONNECT tunnel via the injector (already in v0.0.0).
- Avoid buffered but yet unused data from getting discarded before full-duplex
forwarding of connections.
- ouinet v0.17.1

## v0.17.0 (2022-03-03)

Release v0.17.0.
This release fixes many networking bugs related with error reporting and
connection close, which result in better use of connections and less chances
of them getting stuck and timing out.  Particularly, the client stops reading
from the Injector when the user agent closes the connection, and traffic
forwarding (i.e. in Proxy HTTPS and bridge client tunnels) reacts immediately
to remote connection close.  Also, some outdated code has been updated when
handling Proxy CONNECT responses at the client.
Enhancements:
- Update readme regarding proxy settings when testing Ouinet in a browser.
- Better reporting of closed connections and local proxy auth failures at the
client.
Bug fixes:
- Cancel reading from the Injector job if user agent closes connection
(otherwise the client would continue until all requests complete, using
extra bandwidth, and memory until then).  Incomplete results are stored.
- Close both ends of full duplex forwarding on read or write error, thus
avoiding forwarded connections (Proxy HTTPS and bridge client) from getting
stuck and timing out on remote close.
- A "Software caused connection abort" error is triggered at the end of
loading an incomplete response body from the local cache, instead of
pretending that complete retrieval succeeded.
- Correct handling of Proxy CONNECT responses: no longer drop error message
bodies, succeed and do not expect body on other non-200 success codes.
- Do not reuse Proxy connection to origin if already released, avoid crash.
- Close user agent connection upon an error, if it was already written to.
- Correct handling of new session (or slurped response) not reading
anything (instead of crashing or returning wrong error).  Use consistent
error when closed or uninitialized.
- ouinet v0.17.0

## v0.16.0 (2022-02-08)

Release v0.16.0.
This release adds reporting of external and public UDP endpoints, along with
other client front-end and status API refinements.  More cases of buggy UPnP
port mappings are handled, and they are better reported and logged.  Many
timeouts and cancellations have been added for network operations to avoid
them from getting stuck after an error or a shutdown signal.  These operations
are also better logged and easier to pinpoint when debugging is on.
Features:
- Report external and public UDP endpoints in client front-end and status API,
retrieved from UPnP (if available) and the BitTorrent DHT, respectively.
Enhancements:
- Only build either release or debug Android library depending on build mode.
This cuts build time in half.
- Better ordering of network status information in client front-end, with more
relevant items first.
- More useful values for `is_upnp_active` in client front-end and status API:
`disabled` if UPnP is not available in the network, `inactive` if it is but
a port mapping could not be added.
- Reuse stale port mapping entries (having the right internal address and
port) which fail to be refreshed (because of a buggy router) and report UPnP
as `active` in that case.
- Add timeouts to many network operations at injector and client to avoid them
from lingering forever until cancelled.  This should avoid many "is still
working" log messages, esp. in long-running processes like injectors.
- More asynchronous tasks have their log messages tagged.
- Much better reporting of UPnP operations in client log messages.  Enhanced
formatting of error messages and HTTP statuses.
- The injector also logs the signed head sent to the client in injection
operations.
- Replace most timeout watchdogs with new, more efficient implementation.
- Many minor code fixes and cleanups esp. in BitTorrent DHT, request parsing,
on cancellation.
Bugfixes:
- Only report actually used local UDP endpoints in client front-end and status
API.
- Do not report as local UDP endpoints those which failed to get bound by DHT
code.
- Fix task logging tags which were silently discarded due to implicit type
conversions.
- Close connections to other clients when client is shut down, to avoid
getting stuck.
- Close full duplex connections in client and injector on shut down, to avoid
getting stuck.
- Do not send an error from injector to client in a Proxy request if data was
already sent.
- Fix possible segmentation fault when destroying an already stopped watchdog.
- ouinet v0.16.0

## v0.15.0 (2021-12-13)

Release v0.15.0.
This release fixes some Android-specific breakage when loading native
libraries, which caused issues like the client getting stuck in "starting"
state with some 64-bit devices, and getting "End of file" errors when serving
the signatures of a cached resource to another client.
For Android applications using the library, they will need to add a dependency
on ReLinker.  Please see the readme for more information.
Bugfixes:
- Use ReLinker to avoid duplicate loading of native libraries under Android
which caused (among others) failed comparisons of error categories in HTTP
store code.
- ouinet v0.15.0

## v0.14.0 (2021-11-30)

Release v0.14.0.
This release includes a few changes and fixes for Android, as well as avoiding
crashes with UPnP on some Android devices.
Enhancements:
- When building using Docker, Debian's packaged OpenJDK is used instead of
relying on Oracle JDK as included in Android IDE.
- Reduce dependencies on Android support libraries.
- Allow running the emulator via the Android build script under a Docker
container (see readme for instructions).
- Other cleanup and minor enhancements to the Android build script: better
comments, let Gradle install dependencies when possible, better
configuration via environment variables.
Bugfixes:
- Fixes many SDK and API version inconsistencies for Android so that the
library can safely be run under Android 4.1 Jelly Bean (API level 16) or
newer.  Versions are documented in `doc/android-sdk-versions.md`.
- Fix a segmentation fault in UPnP code under some Android devices.
- Avoid installing Gradle or Android NDK when not bootstrapping via the
Android build script (e.g. for running the emulator).
- ouinet v0.14.0

## v0.13.0 (2021-10-05)

ouinet v0.13.0
This release makes bootstrapping of the BitTorrent DHT more robust by adding
new bootstrap nodes run by the Ouinet project, and by periodically saving DHT
contacts.  Also, log messages are easier to read and parse with tools like
`grep` thanks to being more consistent all across code, and to including some
extra context in key places.
Enhancements:
- Add `router.bt.ouinet.work:6881` and `routerx.bt.ouinet.work:5060` as
BitTorrent DHT bootstrap nodes.  As with other name-based bootstrap nodes
used by Ouinet, they can be hot-patched via `/etc/hosts`,
personalDNSfilter (Android) or similar.
- Allow specifying port of bootstrap node in BitTorrent DHT code.
- Store BitTorrent DHT contacts periodically to avoid losing them on crash.
- Make log messages more consistent.  The generic format is:
`Prefix: Message[: object][(...|status)][;( key=value)+]`.
- Add (shortened) target in request/response log messages to avoid having to
lookup the target based on the request number.
Bugfixes:
- Fix minor cancellation oversights in BitTorrent DHT contacts store.
- ouinet v0.13.0

## v0.12.1 (2021-07-02)

ouinet v0.12.1
This release fixes a couple of minor bugs, the main one of them preventing the
seeding of content files in a static cache which have a space in their names.
Bugfixes:
- Read a whole line of `body-path` HTTP store files instead of stopping at
the first white space.  This fixes seeding content files in a static cache
which have a space in their names (but not a new line, though this is way
less probable).
- Fix signal to check for error in Injector fresh retrieval.  It may trigger
an assertion error in the unlikely case that getting a connection from the
injector to the origin blocks for 24 hours.
- ouinet v0.12.1

## v0.12.0 (2021-06-29)

Release v0.12.0.
This release greatly increases the speed of distributed cache retrieval by
pre-fetching data blocks in multi-peer downloads.  When pre-fetching from the
same peer, the speedup can reach 10x.
Enhancements:
- Pre-fetch data blocks from same and different peers in multi-peer
downloads.
- Increase some timeouts in multi-peer downloads.
Bugfixes:
- Use a different directory in the Docker container for the static cache,
outside of the program repository.  This avoids issues with mounted repos
and caches (as volumes, bind mounts, etc.), but the setup is incompatible
with v0.11 containers with a static cache.  If you have such a container,
you will need to re-create it.
- ouinet v0.12.0

## v0.11.3 (2021-06-15)

Release v0.11.3.
This release fixes a long-standing bug in the handling of BitTorrent DHT
queries, as well as a couple of bugs introduced in v0.11.2 regarding access to
the local cache.  Hopefully the former should make BT operation much more
stable and efficient.
Bugfixes:
- Correct sending replies to BT DHT queries with an `e` field (error)
instead of `r` (reply).
- Do reset error code after failed lookup in the local cache.
- Do reset error code when computing available body size of local cache
entry without a `body` file, when static cache is disabled.
- ouinet 0.11.3

## v0.11.3-docker1 (2021-06-16)

Wrapper: Change static cache directory.
If it is a mount point (e.g. a bind mount from the host) and the repo is
too (e.g. a volume), files in the repo are mangled.
This is unfortunately not backwards-compatible with v0.11.1 clients, but there
are probably not many such clients in the wild yet.
- ouinet v0.11.3-docker1

## v0.11.2 (2021-06-15)

Release v0.11.2.
This release includes some performance enhancements in BitTorrent code, as
well as fixes to the handling of `HEAD` requests and incomplete responses in
the local cache.
Enhancements:
- Avoid buffer resizes in BitTorrent multiplexer code, resulting in more
than 3x uTP transmission speedup in some devices.
- Avoid unnecessary copies in BitTorrent bencoding and DHT code.
Bugfixes:
- Fix `HEAD` requests served by the local cache including a body, which
broke the subsequent requests.
- Do not use a response from the local cache if it is incomplete and
retrieval from the distributed cache is possible.  This is a temporary fix
until the local cache can take full part in the multi-peer download
process.
- Fix malformed HTTP warning header.
- ouinet v0.11.2

## v0.11.1 (2021-06-10)

Release v0.11.1.
This release fixes signing non-GET/HEAD requests at the injector, as well as
avoiding statically changing proxy settings when loading the `Ouinet` class in
the Android API.  Docker clients also automatically enable the static cache if
available.
Features:
- Enable the static cache in Docker clients, if the directory
`/var/opt/ouinet/client/static-cache` is found to be a static cache root.
Please note that this may also be mounted from the host.
Bugfixes:
- Avoid signing responses for requests which are not GET nor HEAD at the
injector.  The exchange may happen but no signing is done.  Please
consider using Proxy requests for those exchanges instead, to prevent the
injector from seeing potentially private data.
- Do not have the `Ouinet` class in the Android API change HTTP(S) proxy
host and port system properties.  This should be (un)done explicitly by
the API consumer when convenient.
- ouinet v0.11.1

## v0.11.0 (2021-06-02)

Release v0.11.0.
This release contains some features and enhancements that ease the integration
of Ouinet in other applications beyond Web browsers, and facilitate debugging
and testing.  It also uses updated `asio-utp` code that speeds up
transmissions by 20% on slow devices.
Features:
- Optional "private caching" mode in client which allows caching responses
marked as private or being the result of a request containing an
`Authorization:` header.  Sensitive headers are still removed from
Injector requests, so injectors and origin sites may need special
configuration to support this.  Please use with care.
- Ability to get the state of a client via C++, HTTP or Android APIs.
- Allow disabling particular request mechanisms from the Android API.
Enhancements:
- Update `asio-utp` to avoid receive buffer reallocations.
- `debug` options have been replaced with `log-level` options which allow to
specify the log level.  Thus the old `debug` becomes `log-level=debug`.
The wrapper script takes care of upgrading configuration files.
- Allow HEAD requests to go through the Injector.  They cannot be cached
yet because of storage format limitations, though.
- More specific errors from the injector, some of them being proper Ouinet
protocol messages with `X-Ouinet-Version` and `X-Ouinet-Error` headers.
The client has also been adapted to propagate more of these to the agent,
so that it gets more precise errors that can be handled e.g. with a
browser add-on.
Bugfixes:
- Avoid crashing if client BT bootstrap is cancelled when no bootstrap node
was contacted.
- Use `text/plain` for injector ok API (empty) responses instead of
`text/html`.
- ouinet v0.11.0

## v0.10.0 (2021-05-13)

Release v0.10.0.
This release includes many enhancements and fixes, but the most important
changes are (1) the new support for an external, read-only, permanently
announced *static cache* at the client which can be circulated offline in file
storage, and which is used as a fallback to the client's internal local cache,
(2) the removal of the "secure Origin" request method in favor of techniques
like HTTPS Everywhere, and (3) the removal of hard-wired site-specific rules
which negatively impacted access to resources like Mozilla updates and
add-ons.  Injectors can also be run in a restricted mode which only allows
signing certain content.
Features:
- Add support for static cache in the client, including Android API and note
on `READ_EXTERNAL_STORAGE` permission.  Such caches can be created by
copying a client's local cache (e.g. for off-line browsing) or with
specific tools like `ouinet-inject`.
- Allow disabling the HTTP(S) Proxy mechanism of the injector.
- Allow restricting injected URIs to those matching a regexp; implies
disabling Proxy mechanism.
Enhancements:
- Remove hard-wired request routes for specific sites at the client; include
instructions to get equivalent results with user agent configuration.
- Remove the "secure Origin" request method; please use user agent-level
approaches like HTTPS Everywhere.
- Split front-end page in sections, show all information available from the
status API.
- Update browser versions in canonical request `User-Agent:` header to
reduce fingerprinting.
- Ensure that all requests carry a `Host:` header inside of the client.
- Handle `X-HTTP-Method-Override` in request routing.
- Correctly handle `TE` header when caching a page.
- Report remote endpoint associated with stream in request log messages.
- Better error reporting in Proxy and Injector mechanisms of injector.
- Report reason of page not being locally cached (when debugging).
- Avoid some error messages on client termination.
Bugfixes:
- Fix allowing uppercase URL scheme and host name (not a problem with most
browsers).
- Fix corner case in `ConditionVariable` when cancelling after notification.
- Fix protocol version error responses missing `Content-Length`.
- Fix case handling when detecting localhost accesses at the client.
- Fix wrong debug preprocessor check which prevented access to some sites.
- Get and check for URI in stored head on load.
- Remove obsolete options in client and injector.
- Fix readme instructions for configuring client proxy in the browser.
- Update Boost source download location.
- asio-utp: Fix IPv6 remote endpoint retrieval.
- ouinet v0.10.0

## v0.9.9 (2021-03-18)

Release v0.9.9.
This release increases client responsiveness by avoiding some unnecessary
pauses during resource retrieval which caused 3s delays at the beginning of
requests.  It also fixes an issue with UPnP routers not supporting the newer
IGDv2 protocol.
Enhancements:
- Use IGDv1 operations to list UPnP mappings instead of IGDv2 (for increased
compatibility).
Bugfixes:
- Avoid resource retrieval jobs at the client from adding extra start delay
because of their own presence in the list of concurrent jobs.
- Ensure that canonicalized URLs do not contain fragments.
- ouinet v0.9.9

## v0.9.8 (2021-03-12)

Release v0.9.8.
This release fixes some oversights in request and response sanitization, as
well as keep-alive handling.  Other minor enhancements in request and response
handling are included.  Most keep-alive fixes will have effect once we solve
some issues in connection reuse.
Enhancements:
- Injector decouples keep-alive from client requests and origin responses,
increasing the chances of reusing origin connections.  Requests initiated
by the injector always have keep-alive on, even if the client does not
want to.  Unfortunately, other pending issues prevent actual connection
reuse, so this enhancement is not in effect yet.
- Check request in client before trying to connect to the injector.
- Turn "device or resource busy" DNS errors into "host not found".
Bugfixes:
- Increased checks of request targets and `Host:` header fields triggering
some assertions.
- Fix missing filtering of Ouinet headers in requests and responses.
- Do not report false error after serving each front-end request.
- Avoid reusing injection id and time stamp on responses coming from the
same origin connction.  Fortunately the bug did not manifest as connection
reuse is not working yet.
- Fix checking of keep-alive status in client requests and origin
responses (particularly `Connection: close` as HTTP/1.1 enables keep-alive
by default).  Again, not completely in effect until connection reuse is
fixed.
- ouinet v0.9.8

## v0.9.7 (2021-02-22)

Release v0.9.7.
This release enables opening client ports ASAP to minimize the chances of
agents finding the ports unavailable while the client starts.  Also, the API
no longer allows changing the client configuration after it has been created.
Enhancements:
- Listen on client ports immediately, then start injector and cache setup
concurrently to enable accepting incoming connections ASAP.
- Remove client functions to allow changing the injector endpoint and
credentials after creation.  Android API equivalents removed as well.
- ouinet v0.9.7

## v0.9.6 (2021-02-03)

Release v0.9.6.
This enables UPnP mapping handling to cope with buggy IGDs/routers which
ignore requests for refreshing existing mappings, causing the mapping to
intermittently appear and disappear every 3 minutes approximately.  This
should help clients remain reachable to others for seeding content and
bridging connections (both with static and dynamic connectivity).
Enhancements:
- Detect buggy UPnP IGDs and let mappings expire if only such IGDs are found
in the network, then recreate the mappings ASAP.
- Better computation of pauses in UPnP handling code to avoid extra delays.
- ouinet v0.9.6

## v0.9.5 (2021-01-21)

Release v0.9.5.
This enhances the handling of swarms in the client, especially to avoid a
long-running one to accumulate too many swarm entries, which results in the
client very seldom pinging injectors and thus stop announcing itself as a
bridge.
Enhancements:
- Use a limited-size LRU cache to keep swarm entries at the client (to
eventually drop spurious entries).
- Do not ping injectors if a connection to one of them was successfully
established while waiting for the next ping round.
- ouinet v0.9.5

## v0.9.4 (2021-01-07)

Release v0.9.4.
This fixes some issues with the client announcing itself in the bridge swarm,
particularly in the pinging of injectors, which may eventually leave the
client out of the swarm and thus unreachable to others, especially after being
running for an extended period.
Enhancements:
- Ignore DHT martians in the `bt-bep5` utility.  As a result, its output and
that of `ping-swarm` are less cluttered and more useful.
Bugfixes:
- Fix the computation of pauses between pinging injectors at the client, so
that pings are not too far apart, which may cause the client's entry in
the bridge swarm to become questionable.
- Fix the selection of injectors to be pinged in each round, avoiding never
actually pinging interesting injectors if they are located after a certain
point in the internal list of nodes seen in the injector swarm.
- ouinet v0.9.4

## v0.9.3 (2021-01-04)

Release v0.9.3.
This release improves logging of injector and bridge client announcements to
BitTorrent swarms, while fixing some issues which may result in both types of
nodes randomly disappearing from those swarms for a while.  It also makes
nodes more able to recover from local connectivity loss.
The release also fixes an issue affecting new Android builds (and breaking
CI/CD).
Enhancements:
- Better logging of injector pings and BEP5 announcements.
- Shorter interval of announcements to the injector swarm, to avoid the DHT
entry to become "questionable" in BEP5 terms.
Bugfixes:
- Avoid pausing after a successful announcement to the helper/bridge swarm,
so that announcements follow injector pings.
- ouinet v0.9.3

## v0.9.2 (2020-12-18)

Release v0.9.2.
This release makes logging more consistent (especially at the client) and
offers some goodies for testing and debugging the client from its front-end.
Features:
- Get the list of groups announced by the client from its front-end
(`/groups.txt`).
- Toggle log file creation from the client front-end, provide a download
link.
- Enabling the log file enables debugging and restores the previous log
level when disabled, unless the user explicitly changes it in the
meanwhile.
Enhancements:
- More consistent logging: always use the logger (i.e. show DEBUG, WARN,
ERROR, INFO tags), fix the log level of many messages (e.g. show requests
and responses as DEBUG when debugging.
- Use a single log level for all components (i.e. drop the separate log
level for the BEP5/HTTP cache).
Bugfixes:
- Fix content being announced not getting reannounced if reinjected shortly
after purging the local cache, when not debugging.
- Fix BEP5 swarm status not being reported when enabling debugging at
runtime.
- ouinet v0.9.2

## v0.9.1 (2020-12-08)

Release v0.9.1.
This release fixes some build errors in Android and a wrong assertion which
may interfere with debug builds.
- ouinet v0.9.1

## v0.9.0 (2020-12-08)

Release v0.9.0.
This release brings a new implementation of the multi-peer download protocol
with some fixes to response injection.  The new versions of signing and
storage protocols are interim until new versions cleaned up for consistency
are released.
Enhancements:
- New protocol v6 supporting multiple, concurrent, client-to-client
download.  As computation of block signatures also changes, this implies a
new storage format v3.
Bugfixes:
- Fix injection of responses with an empty body.  The block signature
created by the injector was incorrect and caused other clients to get
stuck when retrieving such resources from other clients.  This was
specific to v5 injector implementations.
Please note that this affected redirections in the landing pages of some
websites (e.g. `example.com -> https://www.example.com/`), which rendered
the sites unfit for retrieval from other clients.
- ouinet v0.9.0

## v0.8.1 (2020-11-10)

Release v0.8.1.
This release adds some minor enhancements to help with testing, especially
BEP5 connectivity logging and a new test script to ping peers announcing
themselves at the different Ouinet swarms.
Features:
- Report protocol version number in status API (`ouinet_protocol`).
- New `ping-swarm` script to test the reachability of peers in Ouinet
swarms.  The script is included in the Docker image along with other
testing tools under the `utils` directory.
Enhancements:
- With debugging on, log the actual peer (injector or bridge) used to reach
an injector; also periodically report the number of injectors and bridges
seen in their respective swarms (e.g. to detect blocking of BT traffic).
- ouinet v0.8.1

## v0.8.1-docker1 (2020-11-10)
Docker: Fix silly syntax error.
- Docker: Fix silly syntax error.

## v0.8.1-docker2 (2020-11-10)
Docker: Add missing dependency for `ping-swarm` script.
- Docker: Add missing dependency for `ping-swarm` script.

## v0.8.1-docker3 (2020-11-10)
Docker: Fix dependency for `ping-swarm` script.
The OpenBSD version of Netcat is needed, which is *not* the default in Debian
Buster.  Make the exact version explicit to ensure future operation.
- Docker: Fix dependency for `ping-swarm` script.

## v0.8.0 (2020-09-15)

Release v0.8.0.
This release adds support for multi-peer downloads, enabling clients to
increase retrieval speed for big files by concurrently downloading data for
the same URL from several other clients.
Please note that this release introduces a new protocol version (v5), as well
as HTTP store format (v2), so that data cached by previous versions of the
client will be dropped.
Features:
- Multi-peer downloads: Concurrently download the same data shared by
several clients for the same URL, or common data from the beginning of
unfinished downloads of the URL (like canceled transfers or streamed
videos).  A reference client is chosen which has signatures for the newest
and most complete data for the URL.
- ouinet v0.8.0

## v0.7.5 (2020-09-14)

Release v0.7.5.
This release contains a few fixes regarding cancellation of operations, and an
experimental feature to use DNS over HTTPS (DoH) in Origin requests.
Features:
- Experimental support for resolving names using DNS over HTTPS (DoH) for
the Origin request mechanism.  To enable it, you must provide the client
with an `--origin-doh-base` option and give it a DoH base URL argument
(like `https://mozilla.cloudflare-dns.com/dns-query` as used by Mozilla,
more options [here](https://github.com/curl/curl/wiki/DNS-over-HTTPS)).
DoH requests are handled internally as normal requests (respecting
public/private browsing mode), and responses may be shared to the
distributed cache.
The feature is still slow since no private caching is used, so each
requested URL triggers a full DoH request.
Bugfixes:
- Fix checks for cancellation in several places (SSL client handshake,
session flush, timeouts, fresh injection).
- Check for errors after performing `CONNECT` to Proxy.
- Fix case comparison of `X-Ouinet-Private` header values.
- ouinet v0.7.5

## v0.7.4 (2020-06-05)
Release v0.7.4.
Minor fixes and enhancements in client logging.
- Release v0.7.4.

## v0.7.3 (2020-06-03)
Release v0.7.3.
This release contains many minor (and not so minor) enhancements,
optimizations and fixes after some intensive internal testing.
Enhancements:
- Improve handling of temporary connectivity losses regarding UPnP, uTP
transport and ouiservice listener.
- Contact known and local peers while DHT lookups take place, to fail early
if they are not available.
- Fetch response head from peers in parallel after connecting, to avoid
timing out on peers not having a resource we can use.
- Fine-tuning of connection and head retrieval operation timeouts.
- Better logging in cache client and announcer.
Bugfixes:
- Assorted fixes to DHT group loading, addition and removal, announcement
stop on local cache purge, and readdition of removed entries.
- Fixes to several timeouts and deadlocks.
- Release v0.7.3.

## v0.7.2 (2020-05-20)
Release v0.7.2.
Enhancements:
- Add a technical white paper with a detailed description and specification
of Ouinet's architecture, components and protocols.
- Update the request/response flow diagram to reflect current Ouinet
architecture.
- Cleanup cache code to remove unused abstractions.
Bugfixes:
- Fix responses with an empty body loaded from signed HTTP storage missing
the block signature in the last chunk header.
- Release v0.7.2.

## v0.7.1 (2020-04-20)
Release v0.7.1.
Features:
- Automatic, periodic garbage collection of local cache entries older than
the maximum cache age.
- Ability to purge the local cache via client API and front-end.
- Approximate local cache size can be retrieved via the client API and
front-end.
Enhancements:
- Unify client endpoints to `127.0.0.1:8077` (HTTP proxy) and
`127.0.0.1:8078` (front-end) by default and enable both by default.
- Update readme to remove obsolete information and better help testing.
- More efficient URI swarm name computations.
- Use newer OpenSSL library.
- Use a non-dummy version number in the client API, and have proper release
notes. `;)`
Bugfixes:
- Avoid issues with BitTorrent bootstrap nodes resolving to an IPv6 address
first.
- Avoid the agent from furiously retrying when there is no fresh mechanism
available.
- Other fixes for Android and asynchronous operations, timeouts and
cancellations.
- Release v0.7.1.

## v0.7.0 (2020-03-24)
Update version for Docker.
- Update version for Docker.

## v0.6.1 (2020-03-16)
Enable logging to file using the CENO extension
- Enable logging to file using the CENO extension

## v0.6.0 (2020-03-10)
Update version for Docker.
- Update version for Docker.

## v0.5.0 (2020-02-28)
Update version for Docker.
- Update version for Docker.

## v0.4.3 (2020-01-16)
Update version for Docker.
- Update version for Docker.

## v0.4.2 (2019-12-20)
Let users specify which swarm to use from Bep5Client::connect
When constructing Bep5Client. This is mostly useful for debugging.
- Let users specify which swarm to use from Bep5Client::connect

## v0.4.1 (2019-12-13)
Merge work to have clients proxy requests to injectors
- Merge work to have clients proxy requests to injectors

## v0.4.0 (2019-12-02)
Update version for Docker.
- Update version for Docker.

## v0.3.7 (2019-11-22)
Update version for Docker.
- Update version for Docker.

## v0.3.6 (2019-11-21)
Update version for Docker.
- Update version for Docker.

## v0.3.5 (2019-11-21)
Update version for Docker.
- Update version for Docker.

## v0.3.4 (2019-11-18)
Update version for Docker.
- Update version for Docker.

## v0.3.3 (2019-11-08)
Update version for Docker.
- Update version for Docker.

## v0.3.2 (2019-11-06)
Better human-readable client error message for injector errors.
Also the function may get injector errors other than version incompatibility.
- Better human-readable client error message for injector errors.

## v0.3.1 (2019-11-05)
Have client check protocol version in BEP5/HTTP requests.
- Have client check protocol version in BEP5/HTTP requests.

## v0.3.0 (2019-11-05)
Update HTTP signatures protocol version number to 2.
No changes exist at all regarding version number 1.  This updates just
synchronizes the version number with the one sent by clients.
- Update HTTP signatures protocol version number to 2.

## v0.2.0 (2019-10-28)
Update signed HTTP head version to 1.
Update documents and tests too.
- Update signed HTTP head version to 1.

## v0.1.5 (2019-10-28)
Add MD5 hash of JSON parsing library source.
- Add MD5 hash of JSON parsing library source.

## v0.1.4 (2019-10-25)
Dockerfile: Retrieve Boost license from build directory.
- Dockerfile: Retrieve Boost license from build directory.

## v0.1.3 (2019-08-28)
Fix Base64 decoding function dropping too many trailing null chars.
The new implementation may not be bullet-proof for malformed encoded strings,
but it should not break anything and still work as expected for well-formed
encoded strings (i.e. having one or two trailing `=` characters).
- Fix Base64 decoding function dropping too many trailing null chars.

## v0.1.2 (2019-08-27)
Merge branch 'http-rolling-sigs'.
This includes a couple of proposals for supporting rolling HTTP
signatures, i.e. signatures on partial body data to allow verified streaming
of cached content between clients.
- Merge branch 'http-rolling-sigs'.

## v0.1.1 (2019-08-07)
Update version for Docker.
- Update version for Docker.

## v0.1.1-docker1 (2019-08-07)
Wrapper: missing line continuations.
- Wrapper: missing line continuations.

## v0.1.0 (2019-07-24)
Docker: include Boost ASIO and ASIO SSL libs from build tree.
- Docker: include Boost ASIO and ASIO SSL libs from build tree.

## v0.0.36 (2019-04-29)
Refactor Injector to not do any DHT operations
It was making it very unresponsive
- Refactor Injector to not do any DHT operations

## v0.0.35 (2019-04-24)
Update version for Docker
- Update version for Docker

## v0.0.34 (2019-04-18)
Allow zero BEP44 index capacity
In this case, the BEP44 index does not even create an updater.  This is the
default for the injector, to generally avoid republishing entries whose data
may no longer be seeded by the injector nor any client.
- Allow zero BEP44 index capacity

## v0.0.34ilog (2019-04-23)
Add logging in injector.cpp
- Add logging in injector.cpp

## v0.0.33 (2019-04-08)
Fix function name in TODO message
- Fix function name in TODO message

## v0.0.32 (2019-03-29)
Update BEP44 LRU entries even if they are still found in the DHT
Otherwise we get stuck into checking the same entry until it vanishes from the
DHT or is replaced by one with a greater sequence number.
- Update BEP44 LRU entries even if they are still found in the DHT

## v0.0.31 (2019-03-26)
Fix reading from disk in PersistenLruCache
- Fix reading from disk in PersistenLruCache

## v0.0.30 (2019-03-14)
Merge branch 'bep44-updater-capacity'
This adds `index-bep44-capacity` options both to client and injector to
configure the capacity of the local part of the BEP44 index (both in memory
and in persistent storage).
- Merge branch 'bep44-updater-capacity'

## v0.0.29 (2019-03-05)
There is no `tls:` endpoint parsing, only `tcp:`
Adjust help messages and Docker wrapper script.
- There is no `tls:` endpoint parsing, only `tcp:`

## v0.0.28 (2019-03-01)
Downgrade required C++ standard back to C++14
The version of CMake in both Vagrant and Docker (i.e. Debian Stretch) does not
accept the C++17 standard, so builds broke.  Using the C++14 standard instead
results in a successful build without issues under those platforms, so I am
downgrading it until a compelling reason is provided (and documented in the
commit message) to require upgrading the aforementioned platforms.
- Downgrade required C++ standard back to C++14

## v0.0.27 (2019-02-14)
Group related command-line options together
It would be even better if Boost allowed to insert option grouping heading
lines, but it seems it does not.
- Group related command-line options together

## v0.0.26 (2019-02-11)
Restore accidentally disabled B-tree index creation in injector
- Restore accidentally disabled B-tree index creation in injector

## v0.0.26-docker1 (2019-02-12)
Only copy from I2P backup key to i2pd's if the former exists
- Only copy from I2P backup key to i2pd's if the former exists

## v0.0.25 (2019-02-08)
Add optional memory limit to Docker Compose deployments
- Add optional memory limit to Docker Compose deployments

## v0.0.24 (2019-01-16)
Some pending renames of "db" to "index"
Basically in comments and log messages, plus some file rename.
- Some pending renames of "db" to "index"

## v0.0.24-docker1 (2019-01-17)
Update version for Docker
- Update version for Docker

## v0.0.24-docker2 (2019-01-17)
Fetch Git tags for checking out version in Docker file
Previously one would need to use a commit reference (or a branch name, which
should not be used for this).
- Fetch Git tags for checking out version in Docker file

## v0.0.23 (2018-12-19)
Merge branch 'external-ca-certs'
- Merge branch 'external-ca-certs'

## v0.0.22 (2018-12-10)
Merge branch 'key-vs-url'
This tries to stablish a clear separation between URLs and db keys.
Hopefully, URLs should be made canonical before attempting a lookup or
insertion, and the formatting of the key in general should be easily
changeable (instead of chasing all the places where a URL is used directly as
a key.
URL canonicalization now takes place in `ouinet::util::canonical_url`, and key
formatting in `ouinet::key_from_http_req`.
- Merge branch 'key-vs-url'

## v0.0.21 (2018-12-06)
Update asio-ipfs module
To throttle dht upload speed. For this to take effect in an existing
builds. Delete the
"<build-dir>/modules/asio-ipfs/CMakeFiles/go-ipfs-complete"
directory
- Update asio-ipfs module

## v0.0.20 (2018-11-28)
Merge branch 'fix-conf-ports'
This adds saner port defaults for Docker images, as well as enabling TLS at
the injector and generating a random credential password.
The readme and Docker Compose files are updated to reflect the changes, along
with some instructions for hosts which do not support Docker host networking.
- Merge branch 'fix-conf-ports'

## v0.0.19 (2018-11-09)
Disable origin access on Android
Temporarily, while the app is being tested
- Disable origin access on Android

## v0.0.18 (2018-11-02)
Remove PID file functionality
It was not really useful since (i) neither the client nor injector processes
do fork, (ii) Android and Docker have their own mechanisms for not running two
instances concurrently, (iii) the exit codes (both from programs and Docker
wrapper script) are quite reliable now, so auto-restarting with Docker or some
monitor should work, and (iv) it made development testing quite uncomfortable.
- Remove PID file functionality

## v0.0.17 (2018-11-01)
Use HTTP access for I2PD Git repository
- Use HTTP access for I2PD Git repository

## v0.0.16 (2018-10-02)
Build debug-enabled Docker image with `OUINET_DEBUG=yes` argument
With that enabled, debugging symbols are left in binaries and the program is
run under `gdb` with a backtrace dump on exit.
- Build debug-enabled Docker image with `OUINET_DEBUG=yes` argument

## v0.0.15 (2018-09-26)
Fix: Keep connections alive if `Connection: keep-alive` is present
- Fix: Keep connections alive if `Connection: keep-alive` is present

## v0.0.14 (2018-09-20)
Only seed body in client after receiving injected response
The code was still seeding HTTP head+body after the head was moved into the
descriptor.
- Only seed body in client after receiving injected response

## v0.0.13-1 (2018-09-05)
Fix case of `network-uri` build options
- Fix case of `network-uri` build options

## v0.0.13 (2018-09-05)
Merge branch 'get-descriptor'
This adds a new API endpoint where one can send a GET request with a URI as a
query argument and get back the corresponding URI descriptor in the cache, if
present.
- Merge branch 'get-descriptor'

## v0.0.12 (2018-08-30)
This should fix the assertion from inside Yield
Probably caused by trying to use a "moved from" Yield
by the coroutine spawned in the `start_timing` function.
- This should fix the assertion from inside Yield

## v0.0.11 (2018-08-29)
Mostly better logging and timeout for fetch_http_* functions
- Mostly better logging and timeout for fetch_http_* functions

## v0.0.10-docker1 (2018-07-31)
Client: don't report ssl::error::stream_truncated from clients
https://github.com/boostorg/beast/issues/915#issuecomment-348268391
- Client: don't report ssl::error::stream_truncated from clients

## v0.0.10-docker2 (2018-08-22)
Update asio-ipfs module (bumped go-ipfs version)
- Update asio-ipfs module (bumped go-ipfs version)

## v0.0.9-docker1 (2018-07-09)
New example configuration using Origin and Proxy request mechanisms
Instead of using the Injector mechanism with disabled caching of requests like
HEAD, it uses the Origin and Proxy mechanisms.
The Origin mechanism is disabled by default, and the Proxy mechanism is
enabled by default, which is the configuration most similar to the previous
one.
- New example configuration using Origin and Proxy request mechanisms

## v0.0.8-docker1 (2018-06-27)
Do not move certificate chain into LRU cache
It had no effect in any case but it was bad style since the chain was being
used right after.
- Do not move certificate chain into LRU cache

## v0.0.7-docker1 (2018-06-25)
Use standard library regular expressions instead of Boost's
Fixes #20.
- Use standard library regular expressions instead of Boost's

## v0.0.6-docker1 (2018-06-21)
Increment min required Boost version 1.62 -> 1.67
- Increment min required Boost version 1.62 -> 1.67

## v0.0.6-docker2 (2018-06-21)
Use anonymous HTTPS URL for ASIO-IPFS submodule
- Use anonymous HTTPS URL for ASIO-IPFS submodule

## v0.0.5-android (2018-04-18)
Add debug output to the Client
- Add debug output to the Client

## v0.0.5-docker1 (2018-05-03)
Merge branch 'docker-wrapper-fixes'
This changes the name of the wrapper script to just ``ouinet`` (since it is
not really dependent on Docker) and makes it executable from whatever
directory.
- Merge branch 'docker-wrapper-fixes'

## v0.0.5-docker2 (2018-05-03)
Parse ``--repo`` arguments in wrapper script
This allows the user to specify a different repository directory and have the
wrapper script use it instead of the hardwired default one.
- Parse ``--repo`` arguments in wrapper script

## v0.0.5-docker3 (2018-05-04)
Check for ``--help`` argument in wrapper script
To allow trivial usage to get help about command line without yet messing with
repositories.
- Check for ``--help`` argument in wrapper script

## v0.0.5-docker (2018-04-24)
Merge pull request #17 from equalitie/fetch-end-of-stream
Ignore end_of_stream error when fetching a http page
I tested and the patch resolves the issue now I get 200 and the correct body from the client.
- Merge pull request #17 from equalitie/fetch-end-of-stream

## v0.0.4-android (2018-04-18)
Client now starts seeding content when it receives it from the injector
Until now, the client only started seeding a content when
it obtained it from the cache.
- Client now starts seeding content when it receives it from the injector

## v0.0.3-android (2018-04-18)
Client now starts seeding content when it receives it from the injector
Until now, the client only started seeding a content when
it obtained it from the cache.
- Client now starts seeding content when it receives it from the injector

## v0.0.2-android (2018-04-12)
Update ipfs-cache submodule
Contains a fix to a nasty ipfs-cache::injector bug.
- Update ipfs-cache submodule

## v0.0.1-android (2018-04-10)
Minor: remove typedef
- Minor: remove typedef

## v0.0.0-android (2018-03-29)
Move JNI related code out from MainActivity
- Move JNI related code out from MainActivity

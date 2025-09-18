# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).


## Unreleased

### Added

- New client configuration option `allow-private-targets` to permit requests
of services running in local/private networks, e.g. 192.168.1.13, 10.8.0.2,
172.16.10.8, etc. !133


## [v1.4.0](https://gitlab.com/equalitie/ouinet/-/releases/v1.4.0) - 2025-09-03

### Added

- Two new JNI methods `getProxyEndpoint` and `getFrontendEndpoint` that are
useful to connect with the Ouinet service when the port selection is delegated
to the OS. !125

### Fixed

- Portal functions of the front-end were changed to pass and check `upnps_ptr`,
instead of attempting to de-reference it on the fly, just to prevent issues
when the pointer is null. !131

### Removed

- Remove obsolete code from `OuinetNotification` and its related components.
MR !129

### Security

- Add authentication for the Ouinet proxy requests and also exposes methods
in JNI that can be used in Android. MR !126
- Improve the validations applied to determine if a target is local, private
or public, which determines if it's allowed to be injected. MR !127
- Set explicit limits to the decoding of bencode strings, integers, lists
and dictionaries. MR !130


## [v1.3.1](https://gitlab.com/equalitie/ouinet/-/releases/v1.3.1) - 2025-08-05

### Changed

- Groups and metrics API structure is now unified.
- Linux and Android releases are built with level 3 of compiler optimizations.
- The default Boost version used by Ouinet is now 1.88.0.
- Support for building Ouinet on iOS.


## [v1.3.0](https://gitlab.com/equalitie/ouinet/-/releases/v1.3.0) - 2025-07-07

### Added

- Support for building Ouinet on MacOS with XCode 16.
- Groups API in the front-end interface; Including methods to mark DhtGroups
as pinned, which means that they will be excluded from Purging and Garbage
Collection operations.

### Changed

- Metrics improvements; Rotate DRUID at same time for all sessions. Only send
metrics records on every hour. Do not send timestamps with metrics.


## [v1.2.1](https://gitlab.com/equalitie/ouinet/-/releases/v1.2.1) - 2025-06-06

### Added

- Support for building Android and Linux binaries with ASan (address
sanitizer) capabilities.

### Changed

- Updated Nexus publishing URLs to point to Central Sonatype.
- Minor changes to the Ouinet CMake scripts to make them more compatible
with other operating systems.
- Upgrades Ouinet from depending on `cpp-netlib/uri` to the recommended
replacement, `cpp-netlib/url`.


## [v1.2.0](https://gitlab.com/equalitie/ouinet/-/releases/v1.2.0) - 2025-05-20

### Added

- Privacy-respect metrics collection on the client side.
- Logging capabilities on the injector side.

### Changed

- Docker files use now `rust:slim-bookworm` for builder and `debian:slim-bookworm`
for production images.

### Fixed

- Use of destroyed `_cancel` after yield in `bittorrent/dht`.


## [v1.1.2](https://gitlab.com/equalitie/ouinet/-/releases/v1.1.2) - 2025-05-13

### Fixed

- Obsolete sections of the Ouinet wrapper script were removed to prevent
crashes when starting Docker clients.

### Changed

- Update gradle-nexus.publish-plugin to 2.0.0.
- Add full example client config in repos directory.
- Migrate old release notes to the new changelog format.


## [v1.1.1](https://gitlab.com/equalitie/ouinet/-/releases/v1.1.1) - 2025-05-02

### Fixed

- Add conditionals to use `BUILD_JOB_SERVER_AWARE` only when CMake is equal or
greater than v3.28.

### Changed

- Use CMake v3.31.7 to build Ouinet in the production Docker images.


## [v1.1.0](https://gitlab.com/equalitie/ouinet/-/releases/v1.1.0) - 2025-05-01

### Added

- Support in CMake files to build Ouinet for Windows.
- New CI pipeline to build Ouinet and run the unit tests in Windows.
- Docker files to generate Windows builder images.
- Script to install a specific version of CMake in Linux builder images.

### Changed

- `file_io` component uses now `asio::stream_file` as its main backend
in Windows.
- Minor changes were applied to the injector and client sources to make
them work in Windows.
- CI Linux jobs are using now CMake 3.31.7 to build Ouinet.

### Fixed

- Unit tests are now bulding and passing in Windows.
- Increased Header size limit in the HTTP response reader.


## [v1.0.0](https://gitlab.com/equalitie/ouinet/-/releases/v1.0.0) - 2025-04-14

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


## [v0.21.9](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.9) - 2023-03-28

### Changed

- Introduce callbacks to start and stop threads.
- Allow consuming application to join the `start` or `stop` threads by returning
the thread when calling the `start`, `stop`, `startup`, and `shutdown` methods.
- Remove dependency on activity in `OuinetBackground`.

### Fixed

- Only send pending intents to notification when Ouinet client state changes.
- Call `getState()` on Ouinet object instead of just `state`, which appeared to
be crashing the client.


## [v0.21.8](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.8) - 2023-03-07

### Added

- Add new `max-simultaneous-announcements` option, which defines the number
of simultaneous BEP5 announcements to the DHT. Default for Android
devices is 1 and for the desktop ones is 16.

### Changed

- Spawning batches of announcement as co-routines instead of processing them
sequentially.


## [v0.21.7](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.7) - 2023-03-06

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


## [v0.21.6](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.6) - 2023-02-17

### Fixed

- Fix needed for releasing v0.21.6 ouinet android library. This fixes the
signing of AARs when publishing to Sonatype since gradle 7 changed where
task names are found and became more opinionated on how to find sub-strings
in an array of strings, instead match full task name.


## [v0.21.5](https://gitlab.com/equalitie/ouinet/-/releases/v0.21.5) - 2023-01-04

### Fixed

- Reverts changes made in v0.21.3 and v0.21.4 and applies a new fix for a
crash in the Android library when the stop methods are invoked from a
non-stopped state or when the Ouinet service is switching between WiFi and
mobile connections.


## [v0.21.4](https://gitlab.com/equalitie/ouinet/-/tags/v0.21.4) - 2023-01-03

### Fixed

- Fixes a memory leak in the Android library when
the Ouinet service is restarted by changes in network connectivity,
e.g. switching between WiFi and mobile connections.
- Removes unused broadcast recievers that caused memory leak
if they were not unregistered successfully.

## [v0.21.3](https://gitlab.com/equalitie/ouinet/-/tags/v0.21.3) - 2022-10-12

### Fixed

- Fixes a crash in the Android library when the stop
methods are invoked from a non-stopped state or when the Ouinet service
is switching between WiFi and mobile connections.

## [v0.21.2](https://gitlab.com/equalitie/ouinet/-/tags/v0.21.2) - 2022-07-13

#### Added

- Support in build scripts for a new `omni` pseudo-ABI for Android which
produces a single AAR for ARM32, ARM64 and x64 ABIs.  Useful for creating
Android app bundles (AAB packages).

### Changed

- The OpenSSL build dependency in Android has been updated to version 1.1.1q.
- If in verbose mode, the client now logs when a group is newly announced, or
stopped being announced.

### Fixed

- Fix some functions triggering a cancel object passed by reference by the
caller.
- Fix many error returns not checking for cancellation.
- Fix some cancellation errors being reported where a timeout was expected.
- Fix some cancellation errors in the generic stream implementation being
reported when a "shut down" error was expected.
- Do not override some underlying errors in multi-peer reader with a
cancellation error.

## [v0.21.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.21.1) - 2022-06-15

### Added
- Report when the connection serve operation at the injector leaks an
error (instead of crashing).

### Fixed
- Revert to not leaking an error when reading a request at the injector fails.
This made the injector crash.
- Revert to not leaking an error when the TLS handshake operation fails.  This
was innocuous but logged an excessively alarming error message.

## [v0.21.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.21.0) - 2022-06-14

### Added
- New `bt-bootstrap-extra` option (for client and injector) to add several
extra BitTorrent bootstrap servers.  The option is persistent at the client
and available in the front-end and status API (as `bt_extra_bootstraps`).
When changed via the front-end, it is applied on subsequent runs.  Also
available in the Android wrapper.
- Client front-end inputs can be addressed via URL fragments (with identifier
`input-<INPUT_ID>`).  Some minimal Javascript highlights the selected input.

### Changed
- Properly tag as labels client front-end texts which accompany inputs.
- Add keyboard shortcuts for client front-end inputs missing them.
- Fix missing escaping of values inlined in client front-end HTML.
- Remove internal boilerplate in cancellation and timeout handling by defining
(and using!) functions and macros (and put them in the headers where they
really belong).

### Fixed
- Apply timeouts to response body flush operations at the client.
- Fix timeout not triggering cancellation in response part timed read
operation.
- Do check for timeout events after many async operations.
- Fix error code not getting reset in injector listen loop.

## [v0.20.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.20.0) - 2022-06-01

### Added
- Update Gradle configuration and shell scripts to allow uploading a signed
build of the Android library to Maven Central.

### Changed
- Make client local proxy and front-end endpoints configurable in Android.
You should use in your app some non-default values to avoid clashing with
other Ouinet-enabled apps running on the same device.
- Update readme with new instructions for integration in Android apps.
- Report errors in client writing response to cache and user agent.
- Tag long-running operations in client fetch fresh from injector.
- Log client authentication failures at the injector.

## [v0.19.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.19.0) - 2022-05-23

### Added
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

### Changed
- Make `--help` messages more consistent and better structured.
- Make option processing errors more consistent.
- Better logging of code reporting "operation not supported".

## [v0.18.2](https://gitlab.com/equalitie/ouinet/-/tags/v0.18.2) - 2022-04-12

### Changed
- Allow serving to the user agent entries in the local cache with incomplete
or missing body data for `HEAD` requests.

### Fixed
- Fix miscomputing body data size as 0 for static cache entries when the entry
is not also in the internal cache.  This made cache code regard all static
cache entries as incomplete and trigger unneeded multi-peer downloads which
may fail and render the entries in the static cache inaccessible.
- Avoid getting a reader from the internal cache and its body data size from
the static cache, when both entries exist for the same key in the HTTP store.

## [v0.18.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.18.1) - 2022-04-07

### Changed
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

## [v0.18.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.18.0) - 2022-03-29

### Added
- Log counts of forwarded bytes in injector signing and proxy (plain &
tunnel) (always), and in client content serving and bridge tunnel (with
debug on).

### Changed
- Allow the operation of the cache client before bootstrapping the BitTorrent
DHT (it can be used once bootstrapped).
- Update and simplify Guix build environment instructions.

### Fixed
- Fix unhandled exception in local discovery start with no connectivity.
- Other minor cancellation fixes on waiting for the start of components, in
the full-duplex forward operation and elsewhere.

## [v0.17.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.17.1) - 2022-03-08

### Changed
- With debug on, report resulting error code at the end of each connection
served by injector and client.

### Fixed
- Avoid buffered but yet unused data from getting discarded across requests in
the same client-to-client connection (regression in v0.17.0) or
client-to-origin CONNECT tunnel via the injector (already in v0.0.0).
- Avoid buffered but yet unused data from getting discarded before full-duplex
forwarding of connections.

## [v0.17.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.17.0) - 2022-03-03

### Changed
- Update readme regarding proxy settings when testing Ouinet in a browser.
- Better reporting of closed connections and local proxy auth failures at the
client.

### Fixed
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

## [v0.16.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.16.0) - 2022-02-08

### Added
- Report external and public UDP endpoints in client front-end and status API,
retrieved from UPnP (if available) and the BitTorrent DHT, respectively.

### Changed
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

### Fixed
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

## [v0.15.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.15.0) - 2021-12-13

### Fixed
- Use ReLinker to avoid duplicate loading of native libraries under Android
which caused (among others) failed comparisons of error categories in HTTP
store code.

## [v0.14.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.14.0)  2021-11-30

### Changed
- When building using Docker, Debian's packaged OpenJDK is used instead of
relying on Oracle JDK as included in Android IDE.
- Reduce dependencies on Android support libraries.
- Allow running the emulator via the Android build script under a Docker
container (see readme for instructions).
- Other cleanup and minor enhancements to the Android build script: better
comments, let Gradle install dependencies when possible, better
configuration via environment variables.

### Fixed
- Fixes many SDK and API version inconsistencies for Android so that the
library can safely be run under Android 4.1 Jelly Bean (API level 16) or
newer.  Versions are documented in `doc/android-sdk-versions.md`.
- Fix a segmentation fault in UPnP code under some Android devices.
- Avoid installing Gradle or Android NDK when not bootstrapping via the
Android build script (e.g. for running the emulator).

## [v0.13.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.13.0) - 2021-10-05

### Changed
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

### Fixed
- Fix minor cancellation oversights in BitTorrent DHT contacts store.

## [v0.12.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.12.1) - 2021-07-02

### Fixed
- Read a whole line of `body-path` HTTP store files instead of stopping at
the first white space.  This fixes seeding content files in a static cache
which have a space in their names (but not a new line, though this is way
less probable).
- Fix signal to check for error in Injector fresh retrieval.  It may trigger
an assertion error in the unlikely case that getting a connection from the
injector to the origin blocks for 24 hours.

## [v0.12.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.12.0) - 2021-06-29

### Changed
- Pre-fetch data blocks from same and different peers in multi-peer
downloads.
- Increase some timeouts in multi-peer downloads.

### Fixed
- Use a different directory in the Docker container for the static cache,
outside of the program repository.  This avoids issues with mounted repos
and caches (as volumes, bind mounts, etc.), but the setup is incompatible
with v0.11 containers with a static cache.  If you have such a container,
you will need to re-create it.

## [v0.11.3-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.11.3-docker1) - 2021-06-16

### Changed
- Change static cache directory. If it is a mount point (e.g. a bind 
mount from the host) and the repo is too (e.g. a volume), files in the 
repo are mangled.

## [v0.11.3](https://gitlab.com/equalitie/ouinet/-/tags/v0.11.3) - 2021-06-15

### Fixed
- Correct sending replies to BT DHT queries with an `e` field (error)
instead of `r` (reply).
- Do reset error code after failed lookup in the local cache.
- Do reset error code when computing available body size of local cache
entry without a `body` file, when static cache is disabled.

## [v0.11.2](https://gitlab.com/equalitie/ouinet/-/tags/v0.11.2) - 2021-06-15

### Changed
- Avoid buffer resizes in BitTorrent multiplexer code, resulting in more
than 3x uTP transmission speedup in some devices.
- Avoid unnecessary copies in BitTorrent bencoding and DHT code.

### Fixed
- Fix `HEAD` requests served by the local cache including a body, which
broke the subsequent requests.
- Do not use a response from the local cache if it is incomplete and
retrieval from the distributed cache is possible.  This is a temporary fix
until the local cache can take full part in the multi-peer download
process.
- Fix malformed HTTP warning header.

## [v0.11.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.11.1) - 2021-06-10

### Added
- Enable the static cache in Docker clients, if the directory
`/var/opt/ouinet/client/static-cache` is found to be a static cache root.
Please note that this may also be mounted from the host.

### Fixed
- Avoid signing responses for requests which are not GET nor HEAD at the
injector.  The exchange may happen but no signing is done.  Please
consider using Proxy requests for those exchanges instead, to prevent the
injector from seeing potentially private data.
- Do not have the `Ouinet` class in the Android API change HTTP(S) proxy
host and port system properties.  This should be (un)done explicitly by
the API consumer when convenient.

## [v0.11.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.11.0) - 2021-06-02

### Added
- Optional "private caching" mode in client which allows caching responses
marked as private or being the result of a request containing an
`Authorization:` header.  Sensitive headers are still removed from
Injector requests, so injectors and origin sites may need special
configuration to support this.  Please use with care.
- Ability to get the state of a client via C++, HTTP or Android APIs.
- Allow disabling particular request mechanisms from the Android API.

### Changed
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

### Fixed
- Avoid crashing if client BT bootstrap is cancelled when no bootstrap node
was contacted.
- Use `text/plain` for injector ok API (empty) responses instead of
`text/html`.

## [v0.10.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.10.0) - 2021-05-13

### Added
- Add support for static cache in the client, including Android API and note
on `READ_EXTERNAL_STORAGE` permission.  Such caches can be created by
copying a client's local cache (e.g. for off-line browsing) or with
specific tools like `ouinet-inject`.
- Allow disabling the HTTP(S) Proxy mechanism of the injector.
- Allow restricting injected URIs to those matching a regexp; implies
disabling Proxy mechanism.

### Changed
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

### Fixed
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

## [v0.9.9](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.9) - 2021-03-18

### Changed
- Use IGDv1 operations to list UPnP mappings instead of IGDv2 (for increased
compatibility).

### Fixed
- Avoid resource retrieval jobs at the client from adding extra start delay
because of their own presence in the list of concurrent jobs.
- Ensure that canonicalized URLs do not contain fragments.

## [v0.9.8](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.8) - 2021-03-12

### Changed
- Injector decouples keep-alive from client requests and origin responses,
increasing the chances of reusing origin connections.  Requests initiated
by the injector always have keep-alive on, even if the client does not
want to.  Unfortunately, other pending issues prevent actual connection
reuse, so this enhancement is not in effect yet.
- Check request in client before trying to connect to the injector.
- Turn "device or resource busy" DNS errors into "host not found".

### Fixed
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

## [v0.9.7](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.7) - 2021-02-22

### Changed
- Listen on client ports immediately, then start injector and cache setup
concurrently to enable accepting incoming connections ASAP.
- Remove client functions to allow changing the injector endpoint and
credentials after creation.  Android API equivalents removed as well.

## [v0.9.6](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.6) - 2021-02-03

### Changed
- Detect buggy UPnP IGDs and let mappings expire if only such IGDs are found
in the network, then recreate the mappings ASAP.
- Better computation of pauses in UPnP handling code to avoid extra delays.

## [v0.9.5](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.5) - 2021-01-21

### Changed
- Use a limited-size LRU cache to keep swarm entries at the client (to
eventually drop spurious entries).
- Do not ping injectors if a connection to one of them was successfully
established while waiting for the next ping round.

## [v0.9.4](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.4) - 2021-01-07

### Changed
- Ignore DHT martians in the `bt-bep5` utility.  As a result, its output and
that of `ping-swarm` are less cluttered and more useful.

### Fixed
- Fix the computation of pauses between pinging injectors at the client, so
that pings are not too far apart, which may cause the client's entry in
the bridge swarm to become questionable.
- Fix the selection of injectors to be pinged in each round, avoiding never
actually pinging interesting injectors if they are located after a certain
point in the internal list of nodes seen in the injector swarm.

## [v0.9.3](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.3) - 2021-01-04

### Changed
- Better logging of injector pings and BEP5 announcements.
- Shorter interval of announcements to the injector swarm, to avoid the DHT
entry to become "questionable" in BEP5 terms.

### Fixed
- Avoid pausing after a successful announcement to the helper/bridge swarm,
so that announcements follow injector pings.
- ouinet v0.9.3

## [v0.9.2](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.2) - 2020-12-18

### Added
- Get the list of groups announced by the client from its front-end
(`/groups.txt`).
- Toggle log file creation from the client front-end, provide a download
link.
- Enabling the log file enables debugging and restores the previous log
level when disabled, unless the user explicitly changes it in the
meanwhile.

### Changed
- More consistent logging: always use the logger (i.e. show DEBUG, WARN,
ERROR, INFO tags), fix the log level of many messages (e.g. show requests
and responses as DEBUG when debugging.
- Use a single log level for all components (i.e. drop the separate log
level for the BEP5/HTTP cache).

### Fixed
- Fix content being announced not getting reannounced if reinjected shortly
after purging the local cache, when not debugging.
- Fix BEP5 swarm status not being reported when enabling debugging at
runtime.

## [v0.9.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.1) - 2020-12-08

### Fixed
- Fixes some build errors in Android and a wrong assertion which
may interfere with debug builds.

## [v0.9.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.9.0) - 2020-12-08

### Changed
- New protocol v6 supporting multiple, concurrent, client-to-client
download.  As computation of block signatures also changes, this implies a
new storage format v3.

### Fixed
- Fix injection of responses with an empty body.  The block signature
created by the injector was incorrect and caused other clients to get
stuck when retrieving such resources from other clients.  This was
specific to v5 injector implementations.
Please note that this affected redirections in the landing pages of some
websites (e.g. `example.com -> https://www.example.com/`), which rendered
the sites unfit for retrieval from other clients.

## [v0.8.1-docker3](https://gitlab.com/equalitie/ouinet/-/tags/v0.8.1-docker3) - 2020-11-10

### Fixed
- Docker: Fix dependency for `ping-swarm` script.
- The OpenBSD version of Netcat is needed, which is *not* the default in Debian
Buster.  Make the exact version explicit to ensure future operation.

## [v0.8.1-docker2](https://gitlab.com/equalitie/ouinet/-/tags/v0.8.1-docker2) - 2020-11-10

### Fixed
- Docker: Add missing dependency for `ping-swarm` script.

## [v0.8.1-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.8.1-docker1) - 2020-11-10

### Fixed
- Docker: Fix silly syntax error.

## [v0.8.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.8.1) - 2020-11-10

### Added
- Report protocol version number in status API (`ouinet_protocol`).
- New `ping-swarm` script to test the reachability of peers in Ouinet
swarms.  The script is included in the Docker image along with other
testing tools under the `utils` directory.

### Changed
- With debugging on, log the actual peer (injector or bridge) used to reach
an injector; also periodically report the number of injectors and bridges
seen in their respective swarms (e.g. to detect blocking of BT traffic).

## [v0.8.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.8.0) - 2020-09-15

### Added
- Multi-peer downloads: Concurrently download the same data shared by
several clients for the same URL, or common data from the beginning of
unfinished downloads of the URL (like canceled transfers or streamed
videos).
- A reference client is chosen which has signatures for the newest
and most complete data for the URL.

### Changed
- Introduces a new protocol version (v5), as well as HTTP store format (v2), 
so that data cached by previous versions of the client will be dropped.

## [v0.7.5](https://gitlab.com/equalitie/ouinet/-/tags/v0.7.5) - 2020-09-14

### Added
- Experimental support for resolving names using DNS over HTTPS (DoH) for
the Origin request mechanism.  To enable it, you must provide the client
with an `--origin-doh-base` option and give it a DoH base URL argument
(like `https://mozilla.cloudflare-dns.com/dns-query` as used by Mozilla,
more options [here](https://github.com/curl/curl/wiki/DNS-over-HTTPS)).
DoH requests are handled internally as normal requests (respecting
public/private browsing mode), and responses may be shared to the
distributed cache. The feature is still slow since no private caching is
used, so each requested URL triggers a full DoH request.

### Fixed
- Fix checks for cancellation in several places (SSL client handshake,
session flush, timeouts, fresh injection).
- Check for errors after performing `CONNECT` to Proxy.
- Fix case comparison of `X-Ouinet-Private` header values.

## [v0.7.4](https://gitlab.com/equalitie/ouinet/-/tags/v0.7.4) - 2020-06-05

### Fixed
- Minor fixes and enhancements in client logging.

## [v0.7.3](https://gitlab.com/equalitie/ouinet/-/tags/v0.7.3) - 2020-06-03

### Changed
- Improve handling of temporary connectivity losses regarding UPnP, uTP
transport and ouiservice listener.
- Contact known and local peers while DHT lookups take place, to fail early
if they are not available.
- Fetch response head from peers in parallel after connecting, to avoid
timing out on peers not having a resource we can use.
- Fine-tuning of connection and head retrieval operation timeouts.
- Better logging in cache client and announcer.

### Fixed
- Assorted fixes to DHT group loading, addition and removal, announcement
stop on local cache purge, and readdition of removed entries.
- Fixes to several timeouts and deadlocks.

## [v0.7.2](https://gitlab.com/equalitie/ouinet/-/tags/v0.7.2) - 2020-05-20

### Changed
- Add a technical white paper with a detailed description and specification
of Ouinet's architecture, components and protocols.
- Update the request/response flow diagram to reflect current Ouinet
architecture.
- Cleanup cache code to remove unused abstractions.

### Fixed
- Fix responses with an empty body loaded from signed HTTP storage missing
the block signature in the last chunk header.
- Release v0.7.2.

## [v0.7.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.7.1) - 2020-04-20

### Added
- Automatic, periodic garbage collection of local cache entries older than
the maximum cache age.
- Ability to purge the local cache via client API and front-end.
- Approximate local cache size can be retrieved via the client API and
front-end.

### Changed
- Unify client endpoints to `127.0.0.1:8077` (HTTP proxy) and
`127.0.0.1:8078` (front-end) by default and enable both by default.
- Update readme to remove obsolete information and better help testing.
- More efficient URI swarm name computations.
- Use newer OpenSSL library.
- Use a non-dummy version number in the client API, and have proper release
notes. `;)`

### Fixed
- Avoid issues with BitTorrent bootstrap nodes resolving to an IPv6 address
first.
- Avoid the agent from furiously retrying when there is no fresh mechanism
available.
- Other fixes for Android and asynchronous operations, timeouts and
cancellations.

# Appendix A: Git tags reference

This section lists older tags in the repository as a reference to the changes
released before adhering to any consistent release note format. A link to the
comparison between the previous version and the listed version is provided to
give some idea of the changes included in the version. 

## [v0.7.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.7.0) - 2020-03-24

- See changes [v0.6.1...v0.7.0](https://gitlab.com/equalitie/ouinet/-/compare/v0.6.1...v0.7.0)

## [v0.6.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.6.1) - 2020-03-16

- See changes [v0.6.0...v0.6.1](https://gitlab.com/equalitie/ouinet/-/compare/v0.6.0...v0.6.1)

## [v0.6.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.6.0) - 2020-03-10

- See changes [v0.5.0...v0.6.0](https://gitlab.com/equalitie/ouinet/-/compare/v0.5.0...v0.6.0)

## [v0.5.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.5.0) - 2020-02-28

- See changes [v0.4.3...v0.5.0](https://gitlab.com/equalitie/ouinet/-/compare/v0.4.3...v0.5.0)

## [v0.4.3](https://gitlab.com/equalitie/ouinet/-/tags/v0.4.3) - 2020-01-16

- See changes [v0.4.2...v0.4.3](https://gitlab.com/equalitie/ouinet/-/compare/v0.4.2...v0.4.3)

## [v0.4.2](https://gitlab.com/equalitie/ouinet/-/tags/v0.4.2) - 2019-12-20

- See changes [v0.4.1...v0.4.2](https://gitlab.com/equalitie/ouinet/-/compare/v0.4.1...v0.4.2)

## [v0.4.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.4.1) - 2019-12-13

- See changes [v0.4.0...v0.4.1](https://gitlab.com/equalitie/ouinet/-/compare/v0.4.0...v0.4.1)

## [v0.4.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.4.0) - 2019-12-02

- See changes [v0.3.7...v0.4.0](https://gitlab.com/equalitie/ouinet/-/compare/v0.3.7...v0.4.0)

## [v0.3.7](https://gitlab.com/equalitie/ouinet/-/tags/v0.3.7) - 2019-11-22

- See changes [v0.3.6...v0.3.7](https://gitlab.com/equalitie/ouinet/-/compare/v0.3.6...v0.3.7)

## [v0.3.6](https://gitlab.com/equalitie/ouinet/-/tags/v0.3.6) - 2019-11-21

- See changes [v0.3.5...v0.3.6](https://gitlab.com/equalitie/ouinet/-/compare/v0.3.5...v0.3.6)

## [v0.3.5](https://gitlab.com/equalitie/ouinet/-/tags/v0.3.5) - 2019-11-21

- See changes [v0.3.4...v0.3.5](https://gitlab.com/equalitie/ouinet/-/compare/v0.3.4...v0.3.5)

## [v0.3.4](https://gitlab.com/equalitie/ouinet/-/tags/v0.3.4) - 2019-11-18

- See changes [v0.3.3...v0.3.4](https://gitlab.com/equalitie/ouinet/-/compare/v0.3.3...v0.3.4)

## [v0.3.3](https://gitlab.com/equalitie/ouinet/-/tags/v0.3.3) - 2019-11-08

- See changes [v0.3.2...v0.3.3](https://gitlab.com/equalitie/ouinet/-/compare/v0.3.2...v0.3.3)

## [v0.3.2](https://gitlab.com/equalitie/ouinet/-/tags/v0.3.2) - 2019-11-06

- See changes [v0.3.1...v0.3.2](https://gitlab.com/equalitie/ouinet/-/compare/v0.3.1...v0.3.2)

## [v0.3.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.3.1) - 2019-11-05

- See changes [v0.3.0...v0.3.1](https://gitlab.com/equalitie/ouinet/-/compare/v0.3.0...v0.3.1)

## [v0.3.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.3.0) - 2019-11-05

- See changes [v0.2.0...v0.3.0](https://gitlab.com/equalitie/ouinet/-/compare/v0.2.0...v0.3.0)

## [v0.2.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.2.0) - 2019-10-28

- See changes [v0.1.5...v0.2.0](https://gitlab.com/equalitie/ouinet/-/compare/v0.1.5...v0.2.0)

## [v0.1.5](https://gitlab.com/equalitie/ouinet/-/tags/v0.1.5) - 2019-10-28

- See changes [v0.1.4...v0.1.5](https://gitlab.com/equalitie/ouinet/-/compare/v0.1.4...v0.1.5)

## [v0.1.4](https://gitlab.com/equalitie/ouinet/-/tags/v0.1.4) - 2019-10-25

- See changes [v0.1.3...v0.1.4](https://gitlab.com/equalitie/ouinet/-/compare/v0.1.3...v0.1.4)

## [v0.1.3](https://gitlab.com/equalitie/ouinet/-/tags/v0.1.3) - 2019-08-28

- See changes [v0.1.2...v0.1.3](https://gitlab.com/equalitie/ouinet/-/compare/v0.1.2...v0.1.3)

## [v0.1.2](https://gitlab.com/equalitie/ouinet/-/tags/v0.1.2) - 2019-08-27

- See changes [v0.1.1-docker1...v0.1.2](https://gitlab.com/equalitie/ouinet/-/compare/v0.1.1-docker1...v0.1.2)

## [v0.1.1-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.1.1-docker1) - 2019-08-07

- See changes [v0.1.1...v0.1.1-docker1](https://gitlab.com/equalitie/ouinet/-/compare/v0.1.1...v0.1.1-docker1)

## [v0.1.1](https://gitlab.com/equalitie/ouinet/-/tags/v0.1.1) - 2019-08-07

- See changes [v0.1.0...v0.1.1](https://gitlab.com/equalitie/ouinet/-/compare/v0.1.0...v0.1.1)

## [v0.1.0](https://gitlab.com/equalitie/ouinet/-/tags/v0.1.0) - 2019-07-24

- See changes [v0.0.36...v0.1.0](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.36...v0.1.0)

## [v0.0.36](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.36) - 2019-04-29

- See changes [v0.0.35...v0.0.36](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.35...v0.0.36)

## [v0.0.35](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.35) - 2019-04-24

- See changes [v0.0.34ilog...v0.0.35](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.34ilog...v0.0.35)

## [v0.0.34ilog](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.34ilog) - 2019-04-23

- See changes [v0.0.34...v0.0.34ilog](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.34...v0.0.34ilog)

## [v0.0.34](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.34) - 2019-04-18

- See changes [v0.0.33...v0.0.34](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.33...v0.0.34)

## [v0.0.33](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.33) - 2019-04-08

- See changes [v0.0.32...v0.0.33](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.32...v0.0.33)

## [v0.0.32](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.32) - 2019-03-29

- See changes [v0.0.31...v0.0.32](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.31...v0.0.32)

## [v0.0.31](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.31) - 2019-03-26

- See changes [v0.0.30...v0.0.31](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.30...v0.0.31)

## [v0.0.30](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.30) - 2019-03-14

- See changes [v0.0.29...v0.0.30](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.29...v0.0.30)

## [v0.0.29](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.29) - 2019-03-05

- See changes [v0.0.28...v0.0.29](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.28...v0.0.29)

## [v0.0.28](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.28) - 2019-03-01

- See changes [v0.0.27...v0.0.28](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.27...v0.0.28)

## [v0.0.27](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.27) - 2019-02-14

- See changes [v0.0.26-docker1...v0.0.27](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.26-docker1...v0.0.27)

## [v0.0.26-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.26-docker1) - 2019-02-12

- See changes [v0.0.26...v0.0.26-docker1](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.26...v0.0.26-docker1)

## [v0.0.26](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.26) - 2019-02-11

- See changes [v0.0.25...v0.0.26](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.25...v0.0.26)

## [v0.0.25](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.25) - 2019-02-08

- See changes [v0.0.24-docker2...v0.0.25](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.24-docker2...v0.0.25)

## [v0.0.24-docker2](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.24-docker2) - 2019-01-17

- See changes [v0.0.24-docker1...v0.0.24-docker2](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.24-docker1...v0.0.24-docker2)

## [v0.0.24-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.24-docker1) - 2019-01-17

- See changes [v0.0.24...v0.0.24-docker1](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.24...v0.0.24-docker1)

## [v0.0.24](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.24) - 2019-01-16

- See changes [v0.0.23...v0.0.24](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.23...v0.0.24)

## [v0.0.23](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.23) - 2018-12-19

- See changes [v0.0.22...v0.0.23](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.22...v0.0.23)

## [v0.0.22](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.22) - 2018-12-10

- See changes [v0.0.21...v0.0.22](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.21...v0.0.22)

## [v0.0.21](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.21) - 2018-12-06

- See changes [v0.0.20...v0.0.21](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.20...v0.0.21)

## [v0.0.20](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.20) - 2018-11-28

- See changes [v0.0.19...v0.0.20](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.19...v0.0.20)

## [v0.0.19](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.19) - 2018-11-09

- See changes [v0.0.18...v0.0.19](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.18...v0.0.19)

## [v0.0.18](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.18) - 2018-11-02

- See changes [v0.0.17...v0.0.18](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.17...v0.0.18)

## [v0.0.17](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.17) - 2018-11-01

- See changes [v0.0.16...v0.0.17](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.16...v0.0.17)

## [v0.0.16](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.16) - 2018-10-02

- See changes [v0.0.15...v0.0.16](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.15...v0.0.16)

## [v0.0.15](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.15) - 2018-09-26

- See changes [v0.0.14...v0.0.15](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.14...v0.0.15)

## [v0.0.14](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.14) - 2018-09-20

- See changes [v0.0.13-1...v0.0.14](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.13-1...v0.0.14)

## [v0.0.13-1](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.13-1) - 2018-09-05

- See changes [v0.0.13...v0.0.13-1](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.13...v0.0.13-1)

## [v0.0.13](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.13) - 2018-09-05

- See changes [v0.0.12...v0.0.13](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.12...v0.0.13)

## [v0.0.12](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.12) - 2018-08-30

- See changes [v0.0.11...v0.0.12](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.11...v0.0.12)

## [v0.0.11](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.11) - 2018-08-29

- See changes [v0.0.10-docker2...v0.0.11](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.10-docker1...v0.0.11)

## [v0.0.10-docker2](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.10-docker2) - 2018-08-22

- See changes [v0.0.10-docker1...v0.0.10-docker2](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.10-docker1...v0.0.10-docker2)

## [v0.0.10-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.10-docker1) - 2018-07-31

- See changes [v0.0.9-docker1...v0.0.10-docker1](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.9-docker1...v0.0.10-docker1)

## [v0.0.9-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.9-docker1) - 2018-07-09

- See changes [v0.0.8-docker1...v0.0.9-docker1](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.8-docker1...v0.0.9-docker1)

## [v0.0.8-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.8-docker1) - 2018-06-27

- See changes [v0.0.7-docker1...v0.0.8-docker1](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.7-docker1...v0.0.8-docker1)

## [v0.0.7-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.7-docker1) - 2018-06-25

- See changes [v0.0.6-docker2...v0.0.7-docker1](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.6-docker1...v0.0.7-docker1)

## [v0.0.6-docker2](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.6-docker2) - 2018-06-21

- See changes [v0.0.6-docker1...v0.0.6-docker2](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.6-docker1...v0.0.6-docker2)

## [v0.0.6-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.6-docker1) - 2018-06-21

- See changes [v0.0.5-docker3...v0.0.6-docker1](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.5-docker1...v0.0.6-docker1)

## [v0.0.5-docker3](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.5-docker3) - 2018-05-04

- See changes [v0.0.5-docker2...v0.0.5-docker3](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.5-docker2...v0.0.5-docker3)

## [v0.0.5-docker2](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.5-docker2) - 2018-05-03

- See changes [v0.0.5-docker1...v0.0.5-docker2](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.5-docker1...v0.0.5-docker2)

## [v0.0.5-docker1](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.5-docker1) - 2018-05-03

- See changes [v0.0.5-docker...v0.0.5-docker1](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.5-docker...v0.0.5-docker1)

## [v0.0.5-docker](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.5-docker) - 2018-04-24

- See changes [v0.0.5-android...v0.0.5-docker](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.5-android...v0.0.5-docker1)

## [v0.0.5-android](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.5-android) - 2018-04-18

- See changes [v0.0.4-android...v0.0.5-android](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.4-android...v0.0.5-android)

## [v0.0.4-android](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.4-android) - 2018-04-18

- See changes [v0.0.3-android...v0.0.4-android](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.3-android...v0.0.4-android)

## [v0.0.3-android](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.3-android) - 2018-04-18

- See changes [v0.0.2-android...v0.0.3-android](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.2-android...v0.0.3-android)

## [v0.0.2-android](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.2-android) - 2018-04-12

- See changes [v0.0.1-android...v0.0.2-android](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.1-android...v0.0.2-android)

## [v0.0.1-android](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.1-android) - 2018-04-10

- See changes [v0.0.0-android...v0.0.1-android](https://gitlab.com/equalitie/ouinet/-/compare/v0.0.0-android...v0.0.1-android)

## [v0.0.0-android](https://gitlab.com/equalitie/ouinet/-/tags/v0.0.0-android) - 2018-03-29

- See changes [01ed58...v0.0.0-android](https://gitlab.com/equalitie/ouinet/-/compare/01ed58...v0.0.0-android)

## [01ed58](https://gitlab.com/equalitie/ouinet/-/commit/01ed585fedc22ed028cf44d4491f53285ba30666) - 2017-09-21

- Initial commit

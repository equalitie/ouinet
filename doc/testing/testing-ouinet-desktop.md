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

Finally, start the injector.  For the local build you will need to explicitly
point it to the repository created above:

    $ <BUILD DIR>/injector --repo /path/to/injector-repo
    ...
    [INFO] HTTP signing public key (Ed25519): <CACHE_PUB_KEY>
    ...

Note down the `<CACHE_PUB_KEY>` string in the above output since clients will
need it as the *public key for HTTP signatures*.  You may also find that value
in the `ed25519-public-key` file in the injector repository.

When you are done testing the Ouinet injector, you may shut it down by hitting
Ctrl+C.

### Running a test client

To perform some tests using a Ouinet client and an existing test injector, you
first need to know the *injector endpoint* and *credentials*, its *TLS
certificate*, and its *public key for HTTP signatures*.  These use to be
respectively a `tcp:<IP>:<PORT>` string, a `<USER>:<PASSWORD>` string, a path
to a PEM file, and an Ed25519 public key (hexadecimal or Base32).

You need to configure the Ouinet client to use the aforementioned parameters.
If you have a local build, create a copy of the `repos/client` repository
template directory included in Ouinet's source tree:

    $ cp -r <SOURCE DIR>/repos/client /path/to/client-repo

When using a Docker-based client as described above, just run and stop it so
that it creates a default configuration for you.

Now edit `ouinet-client.conf` in the client repository (for Docker, use the
shell container to edit `client/ouinet-client.conf`) and add options for the
injector endpoint (if testing), credentials and public key.  Remember to
replace the values with your own:

    injector-ep = tcp:127.0.0.1:7070
    injector-credentials = injector_user:injector_password
    cache-http-public-key = 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
    cache-type = bep5-http

All the steps above only need to be done once.

Finally, start the client.  For the local build you will need to explicitly
point it to the repository created above:

    $ <BUILD DIR>/client --repo /path/to/client-repo

The client opens a web proxy on local port 8077 by default (see option
`listen-on-tcp` in its configuration file).  When you access the web using
this proxy (see the following section), your requests will go through your
local Ouinet client, which will attempt several mechanisms supported by Ouinet
to retrieve the resource.

When you are done testing the Ouinet client, you may shut it down by hitting
Ctrl+C.

#### A note on persistent options

Please note that a few selected options (like the log level and which request
mechanisms are enabled) are saved when changed, either from the command line
or the client front-end (see below).

On client start, the values of saved options take precedence over those in the
configuration file, but not over those in the command line.  You can use the
`--drop-saved-opts` option to drop the values of saved options altogether.

Please run the client with `--help` to see which options are persistent.

### Testing the client with a browser

Once your local Ouinet client is running (see above), if you have Firefox
installed, you can create a new profile (stored under the `ff-profile`
directory in the example below) which uses the Ouinet client as an HTTP proxy
(listening on `localhost:8077` here) by executing the following commands on
another shell:

    mkdir -p ff-profile
    env http_proxy=http://localhost:8077 https_proxy=http://localhost:8077 \
        firefox --no-remote --profile ff-profile

Otherwise you may manually [modify your browser's settings][Firefox proxy] to
make the client (listening on host `localhost` and port 8077 here) its HTTP
and HTTPS/SSL proxy.

[Firefox proxy]: http://www.wikihow.com/Enter-Proxy-Settings-in-Firefox
    "How to Enter Proxy Settings in Firefox"

Please note that you do not need to change proxy settings at all when using
CENO Extension >= v1.4.0 (see below), as long as your client is listening on
the default address shown above.

To reduce noise in the client log, you may want to disable Firefox's data
collection by unchecking all options from "Preferences / Privacy & Security /
Firefox Data Collection and Use", and maybe entering `about:config` in the
location bar and clearing the value of `toolkit.telemetry.server`.  You can
also avoid some more noise by disabling Firefox's automatic captive portal
detection by changing `network.captive-portal-service.enabled` to `false` in
`about:config`.

If security does not worry you for testing, you can avoid even more noise by
disabling Safe Browsing under "Preferences / Privacy & Security / Deceptive
Content and Dangerous Software Protection" and add-on hotfixes at "Preferences
/ Add-ons / (gear icon) / Update Add-ons Automatically".

Also, if you want to avoid wasting Ouinet network resources and disk space on
ads and similar undesired content, you can install an ad blocker like
[uBlock Origin](https://github.com/gorhill/uBlock).

Once done, you can visit `localhost:8078` in your browser and it should show
you the *client front-end* with assorted information from the client and
configuration tools:

  - To be able to browse HTTPS sites, you must first install the
    *client-specific CA certificate* linked from the top of the front-end page
    and authorize it to identify web sites.  Depending on your browser
    version, you may need to save it to disk first, then import it from
    *Preferences / Privacy & Security / Certificates / View Certificates…*
    into the *Authorities* list.

    The Ouinet client acts as a *man in the middle* to enable it to process
    HTTPS requests, but it (or a trusted injector when appropriate) still
    performs all standard certificate validations.  This CA certificate is
    unique to your device.

  - Several buttons near the top of the page look something like this:

        Injector access: enabled [ disable ]

    They allow you to enable or disable different *request mechanisms* to
    retrieve content:

      - *Origin*: The client contacts the origin server directly via HTTP(S).
      - *Proxy*: The client contacts the origin server through an HTTP proxy
        (currently the configured injector).
      - *Injector*: The client asks the injector to fetch and sign the content
        from the origin server, then it starts seeding the signed content to
        the distributed cache.
      - *Distributed Cache*: The client attempts to retrieve the content from
        the distributed cache.

    Content retrieved via the Origin and Proxy mechanisms is considered
    *private and not seeded* to the distributed cache.  Content retrieved via
    the Injector and Cache mechanisms is considered *public and seeded* to the
    distributed cache.

    These mechanisms are attempted in order according to a (currently
    hard-wired, customizable in the future) *request router configuration*.
    For instance, if one points the browser to a web page which is not yet
    in the distributed cache, then the client shall forward the request to the
    injector.  On success, (A) the injector will fetch, sign and send the
    content back to the client and (B) the client will seed the content to the
    cache.

  - Other information about the cache index is shown next.

**Note:** For a response to be injected, its request currently needs to carry
an `X-Ouinet-Group` header.  The [CENO Extension][] takes care of that
whenever browsing in normal mode, and it does not when browsing in private
mode.  Unfortunately, the Extension is not yet packaged independently and the
only way to use it is to clone its repository locally and load it every time
you start the browser; to do that, open Firefox's *Add-ons* window, then click
on the gears icon, then *Debug Add-ons*, then *Load Temporary Add-on…* and
choose the `manifest.json` file in the Extension's source tree.  Back to the
*Add-ons* page, remember to click on *CENO Extension* and allow *Run in
Private Windows* under *Details*.

[CENO Extension]: https://gitlab.com/censorship-no/ceno-ext-settings/

After visiting a page with the Origin mechanism disabled and Injector
mechanism enabled, and waiting for a short while, you should be able to
disable all request mechanisms except for the Cache, clear the browser's
cached data, point the browser back to the same page and still get its
contents from the distributed cache even when the origin server is completely
unreachable.

### Using an external static cache

Ouinet supports circulating cached Web content offline as file storage and
using a client to seed it back into the distributed cache.  Such content is
placed in a *static cache*, which is read-only and consists of two
directories:

  - A *static cache root* or content directory where data files are stored in
    a hierarchy which may make sense for user browsing.

  - A *static cache repository* where Ouinet-specific metadata and signatures
    for the previous content are kept.

To give your client access to a static cache, use the `cache-static-root` and
`cache-static-repo` options to point to the appropriate directories.  If the
later is not specified, the `.ouinet` subdirectory under the static cache root
is assumed.

Please note that all content in the static cache is permanently announced by
the client, and that purging the client's local cache has no effect on the
static cache.  When cached content is requested from a client, the client
first looks up the content in its local cache, with the static cache being
used as a fallback.

Any user can create such a static cache as a capture of a browsing session by
copying the `bep5_http` directory of the client's repository as a static cache
repository (with an empty static cache root).  We recommend that you purge
your local cache before starting the browsing session to avoid leaking your
previous browsing to other users.

If you are a content provider in possession of your own signing key, please
check the [ouinet-inject][] tool, which allows you to create a static cache
from a variety of sources.

[ouinet-inject]: https://gitlab.com/equalitie/ouinet-inject


## Docker deployment

Ouinet injectors and clients can be run as Docker containers.  An application
configuration file for Docker Compose is included for easily deploying all
needed volumes and containers.

To run a Ouinet node container only a couple hundred MiB are needed, plus the
space devoted to the data volume (which may grow considerably larger in the
case of the injector).

A `Dockerfile` is also included that can be used to create a Docker image
which contains the Ouinet injector, client and necessary software dependencies
running on top of a Debian base system.

### Building the image

Ouinet Docker images should be available from the Docker Hub.  Follow the
instructions in this section if you still want to build the image yourself.
You will need around 3 GiB of disk space.

You may use the `Dockerfile` as included in Ouinet's source code, or you
can just [download it][Dockerfile].  Then build the image by running:

    $ sudo docker build -t equalitie/ouinet:latest - < Dockerfile

That command will build a default recommended version, which you can override
with `--build-arg OUINET_VERSION=<VERSION>`.

After a while you will get the `equalitie/ouinet:latest` image.  Then you may
want to run `sudo docker prune` to free up the space taken by temporary
builder images (which may amount to a couple of GiB).

[Dockerfile]: https://gitlab.com/equalitie/ouinet/-/raw/master/Dockerfile

#### Debugging-enabled image

You can also build an alternative version of the image where programs contain
debugging symbols and they are run under `gdb`, which shows a backtrace in
case of a crash.  Just add `--build-arg OUINET_DEBUG=yes` to the build
command.  We recommend that you use a different tag for these images
(e.g. `equalitie/ouinet:<VERSION>-debug`).

Depending on your Docker setup, you may need to change the container's
security profile and give it tracing capabilities.  For more information, see
[this thread](https://stackoverflow.com/q/35860527).

### Deploying a client

You may use [Docker Compose](https://docs.docker.com/compose/) with the
`docker-compose.yml` file included in Ouinet's source code (or you can just
[download it][docker-compose.yml]).  Whenever you run `docker-compose`
commands using that configuration file, you must be in the directory where the
file resides.

If you want to create a client that seeds a static cache root (see below) from
a directory in the host, check the instructions in `docker-compose.yml`.

If you just plan to **run a single client** with the latest code on your
computer, you should be fine with running the following command:

    $ sudo docker-compose up

That command will create a *data volume*, a main *node container* for running
the Ouinet client or injector (using the host's network directly), and a
convenience *shell container* (see below) to allow you to modify files in the
data volume.  It will then run the containers (the shell container will exit
immediately; this is normal).

To **stop the node**, hit Ctrl+C or run `sudo docker-compose stop`.  Please
note that with the default configuration in `docker-compose.yml`, the node
will be automatically restarted whenever it crashes or the host is rebooted,
until explicitly stopped.

A new client node which starts with no configuration will get a default one
from templates included in Ouinet's source code and it will be missing some
important parameters, so you may want to stop it (see above) and use the
**shell container** (see below) to edit `client/ouinet-client.conf`:

  - If using a local test injector, set its endpoint in option `injector-ep`.
  - Set the injector's credentials in option `injector-credentials`.
  - Unless using a local test injector, set option `injector-tls-cert-file` to
    `/var/opt/ouinet/client/ssl-inj-cert.pem` and copy the injector's TLS
    certificate to that file.
  - Set the public key used by the injector for HTTP signatures in option
    `cache-http-public-key`.
  - To enable the distributed cache, set option `cache-type`.  The only value
    currently supported is `bep5-http`.

After you have set up your client's configuration, you can **restart it**.
The client's HTTP proxy endpoint should be available to the host at
`localhost` port 8077.

If you get a "connection refused" error when using the client's proxy, your
Docker setup may not support host networking.  To enable port forwarding,
follow the instructions in `docker-compose.yml`.

Finally, restart the client container.

[docker-compose.yml]: https://gitlab.com/equalitie/ouinet/-/raw/master/docker-compose.yml

### Using the shell container

You may use the convenience *shell container* to access Ouinet node files
directly:

    $ sudo docker-compose run --rm shell

This will create a throwaway container with a shell at the `/var/opt/ouinet`
directory in the data volume.

If you want to *transfer an existing repository* to `/var/opt/ouinet`, you
first need to move away or remove the existing one using the shell container:

    # mv REPO REPO.old  # REPO is either 'injector' or 'client'

Then you may copy it in from the host using:

    $ sudo docker cp /path/to/REPO SHELL_CONTAINER:/var/opt/ouinet/REPO

### Other deployments

If you plan on running several nodes on the same host you will need to use
different explicit Docker Compose project names for them.  To make the node an
injector instead of a client you need to set `OUINET_ROLE=injector`.  To make
the container use a particular image version instead of `latest`, set
`OUINET_VERSION`.  To limit the amount of memory that the container may use,
set `OUINET_MEM_LIMIT`, but you will need to pass the `--compatibility` option
to `docker-compose`.

An easy way to set all these parameters is to copy or link the
`docker-compose.yml` file to a directory with the desired project name and
populate its default environment file:

    $ mkdir -p /path/to/ouinet-injector  # ouinet-injector is the project name
    $ cd /path/to/ouinet-injector
    $ cp /path/to/docker-compose.yml .
    $ echo OUINET_ROLE=injector >> .env
    $ echo OUINET_VERSION=v0.1.0 >> .env
    $ echo OUINET_MEM_LIMIT=6g >> .env
    $ sudo docker-compose --compatibility up

### Injector container

After an injector has finished starting, you may want to use the shell
container to inspect and note down the contents of `injector/endpoint-*`
(injector endpoints) and `injector/ed25519-public-key` (public key for HTTP
signatures) to be used by clients.  The injector will also generate a
`tls-cert.pem` file which you should distribute to clients for TLS access.
Other configuration information like credentials can be found in
`injector/ouinet-injector.conf`.

Remember that the injector will be available as an HTTP proxy for anyone
having its credentials; if you want to disable this feature, set
`disable-proxy = true`.  You can also restrict the URLs injected to those
matching a regular expression with the `restricted` option.

To start the injector in headless mode, you can run:

    $ sudo docker-compose up -d

You will need to use `sudo docker-compose stop` to stop the container.

To be able to follow its logs, you can run:

    $ sudo docker-compose logs --tail=100 -ft


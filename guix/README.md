# Building Ouinet under a GNU/Guix environment

This assumes that you have a clone of Ouinet source at `/path/to/ouinet`.

To start the environment straight away:

    $ guix time-machine --channels=/path/to/ouinet/guix/channels.scm \
           -- environment -CN --pure \
              --manifest=/path/to/ouinet/guix/manifest.scm \
              --expose=/path/to/ouinet

If you instead want to keep the build profile as `$HOME/env/guix/ouinet/build`
for future invocations (e.g. to avoid its packages from being accidentally
garbage-collected), run:

    $ mkdir -p $HOME/env/guix/ouinet
    $ guix time-machine --channels=/path/to/ouinet/guix/channels.scm \
           -- package --profile=$HOME/env/guix/ouinet/build \
              --manifest=/path/to/ouinet/guix/manifest.scm

You need to do that whenever the `channels.scm` file changes.  Each time you
want to start the environment based on that profile, run:

    $ guix shell -CN --pure \
           --profile=$HOME/env/guix/ouinet/build \
           --expose=/path/to/ouinet

To build source inside of the environment:

    $ export TERM=vt100  # just for colorized warnings and errors
    $ time env SSL_CERT_FILE= GUIX_LOCPATH=$GUIX_ENVIRONMENT/lib/locale LANG=en_US.UTF-8 \
           bash /path/to/ouinet/scripts/build-ouinet-local.sh

The build will fail because of Go's binary interpreter.  Run this command to
fix it, then rerun the command above:

    $ patchelf --set-interpreter \
               "$(patchelf --print-interpreter \
                           ouinet-local-build/CMakeFiles/*/CompilerIdCXX/a.out)" \
               ouinet-local-build/golang/bin/go

The build will be placed under `ouinet-local-build` at the current directory.
Please note that it will only be reusable for further builds as long as the
build environment remains unchanged (i.e. while both `channels.scm` and
`manifest.scm` stay the same).

When running the binaries outside of the container (recommended), remember to
set `SSL_CERT_FILE` like:

    $ env SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt \
          ouinet-local-build/PROGRAM ARGS...

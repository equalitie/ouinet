# Config files

This text explains how each config file was created.

## IPFS config file

The IPFS config file is created automatically by the IPFS' Go code (if it
doesn't exist).

## GNUnet's config file

To create the GNUnet's config file I followed instructions in the
gnunet-c-tutorial.pdf. In short:

Concatenate all "per module" config files into one

```
$ cat <GNUnet-install-dir>/share/gnunet/config.d/*.conf > peer.conf
```

Add a `[hostlist]` section with `SERVERS = ` to prevent bootstrapping.

Add the `GNUNET_TEST_HOME` variable under the `[PATHS]` section.

Modify `PORT` and `UNIXPATH` variables so that different config
files don't interfere with each other.

Comment out all sections which match `[transport-http*]` because these [have
been
problematic](http://lists.gnu.org/archive/html/gnunet-developers/2017-10/msg00007.html).

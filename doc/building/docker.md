## Docker development environment

We provide a *bootstrap* Docker image which is automatically updated with each
commit and provides all prerequisites for building the latest Oiunet desktop
binaries and Android libraries.

To exchange with the container data like Ouinet's source code and cached
downloads and build files, we will bind mount the following directories to
`/usr/local/src/` in the container (some we'll create first):

  - source (assumed to be at the current directory),
  - build (in `../ouinet.build/`),
  - and the container's `$HOME` (in `../ouinet.home/`), where `.gradle`,
    `.cargo`, etc. will reside.

Note that with the following incantations you will not be able to use `sudo`
in the container (`--user`), and that all the changes besides those in bind
mounts will be lost after you exit (`--rm`).

```sh
mkdir -p ../ouinet.build/ ../ouinet.home/
sudo docker run \
  --rm -it \
  --user $(id -u):$(id -g) \
  --mount type=bind,source="$(pwd)",target=/usr/local/src/ouinet \
  --mount type=bind,source="$(pwd)/../ouinet.build",target=/usr/local/src/ouinet.build \
  --mount type=bind,source="$(pwd)/../ouinet.home",target=/mnt/home \
  -e HOME=/mnt/home \
  registry.gitlab.com/equalitie/ouinet:android
```

If you only need to build Ouinet desktop binaries, you may replace the image
name at the end of the command with `registry.gitlab.com/equalitie/ouinet`,
which is much lighter.

After running the command, you should find yourself in a new terminal, ready
to accept the build instructions described elsewhere in the document.

Please consult the GitLab CI scripts to see how to build your own bootstrap
images locally.


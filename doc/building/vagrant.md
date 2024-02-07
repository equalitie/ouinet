## Using a Vagrant environment

One of the easiest ways to build Ouinet from source code (e.g. for development
or testing changes and fixes to code) is using a [Vagrant][] development
environment.

[Vagrant]: https://www.vagrantup.com/

To install Vagrant on a Debian system, run:

    $ sudo apt-get install vagrant

Ouinet's source tree contains a `Vagrantfile` which allows you to start a
Vagrant environment ready to build and run Ouinet by entering the source
directory and executing:

    $ vagrant up

If your Vagrant installation uses VirtualBox by default and you find problems,
you may need to force it to use libvirt instead:

    $ sudo apt-get install libvirt-bin libvirt-dev
    $ vagrant plugin install vagrant-libvirt
    $ vagrant up --provider=libvirt

### Building Ouinet in Vagrant

Enter the Vagrant environment with `vagrant ssh`.  There you will find:

  - Your local Ouinet source tree mounted read-only under `/vagrant`
    (`<SOURCE DIR>`).

  - Your local Ouinet source tree mounted read-write under `/vagrant-rw`.  You
    can use it as a bridge to your host.

  - `~vagrant/build-ouinet-git.sh`: Running this script will clone the Ouinet
    Git repository and all submodules into `$PWD/ouinet-git-source` and build
    Ouinet into `$PWD/ouinet-git-build` (`<BUILD DIR>`).  Changes to
    source outside of the Vagrant environment will not affect this build.

  - `~vagrant/build-ouinet-local.sh`: Running this script will use your local
    Ouinet source tree (mounted under `/vagrant`) to build Ouinet into
    `$PWD/ouinet-local-build` (`<BUILD DIR>`).  Thus you can edit source
    files on your computer and have them built in a consistent environment.

    Please note that this requires that you keep submodules in your checkout
    up to date as indicated previously.

### Accessing Ouinet services from your computer

The Vagrant environment is by default isolated, but you can configure it to
redirect ports from the host to the environment.

For instance, if you want to run a Ouinet client (with its default
configuration) in Vagrant and use it as a proxy in a browser on your computer,
you may uncomment the following line in `Vagrantfile`:

    #vm.vm.network "forwarded_port", guest: 8077, host: 8077, guest_ip: "127.0.0.1"

And restart the environment:

    $ vagrant halt
    $ vagrant up

Then you can configure your browser to use `localhost` port 8077 to contact
the HTTP proxy.


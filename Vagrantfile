#
# == README ==
#
# Based on this descriptor file, Vagrant can setup a standardized, disposable
# virtual machine to build and test ouinet in. This is a virtual machine
# running a platform that we wish to support; with all the dependencies and
# relevant tools installed; and that should theoretically be able to build and
# run ouinet without a hitch.
#
# The virtual machine contains a read-only mount of the source code repository
# containing this Vagrantfile, which you can build and run inside the virtual
# machine in an out-of-source build. By ssh-ing into the virtual machine (see
# below), you can either invoke the one-button shell script to setup a full
# build, or do incremental steps by hand. The source directory is mounted via a
# platform-appropriate network filesystem, so the source can be edited on the
# host machine, and changes are reflected on the virtual machine immediately;
# this directory is mounted as /vagrant inside the virtual machine.
#
# The virtual machines can be created and destroyed with one command each, so
# they are by nature disposable. It is wise not to store any long-term
# configurations or settings inside the virtual machine, but instead to keep
# them as plain as possible; if you manage to mess it up, just throw it away
# and make a fresh one. It is often convenient, if not quite necessary, to
# rebuild the virtual machine whenever the Vagrantfile is updated.
#
# Vagrant by default sets up a NATed network for the virtual machine, with
# limited access from either the host or the rest of the network. We may need
# to reconsider this later for tests that rely on more specific connectivity
# properties.
#
# Usage overview:
#   $ vagrant up
#       This creates the virtual machine, if it doesn't already exist, and
#       starts it. This uses whatever virtual machine software installed on the
#       host machine; if there are multiple, a provider can be selected via
#       command line options.
#   $ vagrant ssh
#       Creates an ssh connection to the virtual machine. This is the easiest
#       way to build and run things inside the virtual machine.
#   $ vagrant destroy
#       Destroys the virtual machine.
# Other vagrant functionality that may be useful includes `vagrant suspend`,
# `vagrant resume`, `vagrant provision`, and `vagrant reload`. RTFM if
# interested.
#
# The home directory contains a script "build-local.sh" that starts a fresh
# build of the ouinet source mounted in /vagrant. This script does not support
# incremental builds for now.
#
# The `vagrant ssh` command is setup to support X11 forwarding. Thus, one can
# run tools like firefox and wireshark inside the virtual machine this way.
# Ideally, a firefox configuration should be ready-to-use to always connect via
# the local ouinet proxy, but for now this is a TODO item.
#
# Alternatively, the ouinet proxy port inside the virtual machine can be mapped
# to a port on the host machine. To enable this, uncomment the "forwarded_port"
# line below.
#

#
# TODO:
# - Consider VM resource allocation, this is overkill
#

Vagrant.configure("2") do |config|
  config.vm.define "linux", primary: true do |vm|
    vm.vm.box = "debian/buster64"

    vm.vm.provider "libvirt" do |v|
      v.memory = 4096
      v.cpus = 4

      v.storage :file, :size => '50G', :device => 'vdb'

      vm.vm.provision "shell", inline: <<-SHELL
        # Vagrant sometimes tries to run this whole script twice, for some reason. Bail out on repeated runs.
        [ -e /scratch ] && exit 0
        mkfs.ext4 /dev/vdb
        mkdir /scratch
        echo "/dev/vdb /scratch ext4 defaults 0 2" >> /etc/fstab
        mount /scratch
        mv /home/vagrant /scratch
        ln -s /scratch/vagrant /home/vagrant
      SHELL
    end

    vm.vm.synced_folder ".", "/vagrant", type: "nfs", mount_options: ["ro", "noac"]
    vm.vm.synced_folder ".", "/vagrant-rw", type: "nfs", mount_options: ["rw", "noac"]

    # Uncomment this line to forward port 8077 on the host machine to port 8077 in the VM, so that you can access the VM ouinet-client from your local browser.
    #vm.vm.network "forwarded_port", guest: 8077, host: 8077, guest_ip: "127.0.0.1"

    vm.ssh.forward_x11 = true

    vm.vm.provision "shell", inline: <<-SHELL
      apt-get update
      apt-get install -y \
        locales \
        aptitude \
        net-tools \
        htop \
        iftop

        # Stop all kinds of tools from warning on missing locales
        echo "en_US.UTF-8 UTF-8" >> /etc/locale.gen
        locale-gen

        # Ensure using that fixed locale even for SSH sessions
        update-locale LANG=en_US.UTF-8
        sed -i -E 's/^(\s*AcceptEnv\b)/#\1/' /etc/ssh/sshd_config
    SHELL

    vm.vm.provision "shell", inline: <<-SHELL
      # Install toolchain and dependencies
      apt-get install -y \
        autoconf \
        automake \
        autopoint \
        build-essential \
        cmake \
        gettext \
        git \
        libssl-dev \
        libtool \
        ninja-build \
        pkg-config \
        python-twisted \
        rsync \
        texinfo \
        wget \
        unzip \
        zlib1g-dev

      # Install testing tools
      echo wireshark-common wireshark-common/install-setuid boolean true | debconf-set-selections
      DEBIAN_FRONTEND=noninteractive apt-get install -y \
        ca-certificates \
        firefox-esr \
        wireshark \
        xauth
      adduser vagrant wireshark

      # Install jdk
      wget 'https://dl.google.com/dl/android/studio/ide-zips/3.4.2.0/android-studio-ide-183.5692245-linux.tar.gz' -O - | tar -axz -C /opt android-studio/jre
      echo 'export PATH=/opt/android-studio/jre/bin:$PATH' >> /etc/profile
    SHELL

    vm.vm.provision "shell", inline: <<-SHELL
      ln -s /vagrant/scripts/build-ouinet-local.sh /home/vagrant/
      ln -s /vagrant/scripts/build-ouinet-git.sh /home/vagrant/
      ln -s /vagrant/scripts/firefox-proxy.sh /home/vagrant/
    SHELL
  end
end

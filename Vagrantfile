Vagrant.configure("2") do |config|
  config.vm.box = "debian/testing64"

  config.vm.provider "libvirt" do |v|
    v.memory = 4096
    v.cpus = 4
  end
  config.vm.provider "virtualbox" do |v|
    v.memory = 4096
    v.cpus = 4
  end

  #config.vm.network "forwarded_port", guest: 8080, host: 8081

  config.vm.synced_folder ".", "/vagrant", mount_options: ["ro"]

  config.vm.provision "shell", inline: <<-SHELL
    apt-get update
    apt-get install -y \
      build-essential \
      pkg-config \
      git \
      wget \
      cmake \
      rsync \
      libtool \
      autoconf \
      automake \
      autopoint \
      texinfo \
      libboost-dev \
      libboost-coroutine-dev \
      libboost-program-options-dev \
      libboost-system-dev \
      libboost-test-dev \
      libboost-thread-dev \
      libboost-filesystem-dev \
      libboost-date-time-dev \
      libgcrypt-dev \
      libidn11-dev \
      libssl-dev \
      libunistring-dev \
      zlib1g-dev \
      locales
    echo "en_US.UTF-8 UTF-8" >> /etc/locale.gen
    locale-gen

  SHELL

  config.vm.provision "shell", inline: <<-SHELL
    ln -s /vagrant/scripts/vagrant-build-local.sh /home/vagrant/build-local.sh
    ln -s /vagrant/scripts/vagrant-build-upstream.sh /home/vagrant/build-upstream.sh
  SHELL
end

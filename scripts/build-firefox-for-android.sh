#!/bin/bash

set -e

DIR=`pwd`
export PATH="$HOME/.cargo/bin:$PATH"

function clean {
    rm -rf $HOME/.android
    rm -rf $HOME/.cargo
    rm -rf $HOME/.mozbuild
    rm -rf $HOME/.multirust
    rm -rf $HOME/.rustup
    rm -rf $HOME/.cache
    rm -rf $DIR/mozilla-central
}

function install_dependencies {
    sudo apt-get update
    sudo apt-get -y install curl mercurial libpulse-dev libpango1.0-dev \
        libgtk-3-dev libgtk2.0-dev libgconf2-dev libdbus-glib-1-dev \
        yasm libnotify-dev clang-4.0
}

function maybe_download_moz_sources {
    # Useful for debuggning when we often need to fetch unmodified versions
    # of Mozilla's source tree (which is about 6GB big).
    local keep_copy=0

    # https://developer.mozilla.org/en-US/docs/Mozilla/Developer_guide/Build_Instructions/Simple_Firefox_for_Android_build
    if [ ! -d mozilla-central ]; then
        if [ -d mozilla-central-orig ]; then
            cp -r mozilla-central-orig mozilla-central
        else
            hg clone https://hg.mozilla.org/mozilla-central
            if [ $keep_copy == "1" ]; then
                cp -r mozilla-central mozilla-central-orig
            fi
        fi
    fi
}

function maybe_install_rust {
    if ! which rustc; then
        # Install rust https://www.rust-lang.org/en-US/install.html
        curl https://sh.rustup.rs -sSf | sh
        rustup update
        # https://bugzilla.mozilla.org/show_bug.cgi?id=1384231
        rustup target add armv7-linux-androideabi
    fi
}

################################################################################
#clean
install_dependencies
(cd $DIR; maybe_install_rust)
(cd $DIR; maybe_download_moz_sources)
################################################################################

cd mozilla-central

# https://developer.mozilla.org/en-US/docs/Mozilla/Developer_guide/Build_Instructions/Simple_Firefox_for_Android_build#I_want_to_work_on_the_back-end
cat > mozconfig <<EOL
# Build Firefox for Android:
ac_add_options --enable-application=mobile/android
ac_add_options --target=arm-linux-androideabi

# With the following Android SDK and NDK:
ac_add_options --with-android-sdk="/home/vagrant/.mozbuild/android-sdk-linux"
ac_add_options --with-android-ndk="/home/vagrant/.mozbuild/android-ndk-r15c"
EOL

# Install some dependencies and configure Firefox build
./mach bootstrap --application-choice=mobile_android --no-interactive

# Note: If during building clang crashes, try increasing vagrant's RAM
./mach build
./mach package


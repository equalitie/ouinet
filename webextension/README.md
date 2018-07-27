# Ouinet WebExtension

## Install web-ext

Use these instructions:

    https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Getting_started_with_web-ext

But note that the `node` in Ubuntu's apt repositories is an old version. Set up
your repos as described here

    https://github.com/nodesource/distributions

and get the latest version or you'll see errors when trying to use `web-ext`.

## Testing:

### PC

    $ cd ouinet/webextension
    $ web-ext run

### Android

    $ # Make sure you have:
    $ # * Firefox/Android installed on your devices
    $ # * web-ext installed on your PC
    $ # * The `adb devices` lists your device
    $ cd ouinet/webextension
    $ web-ext run --target=firefox-android --adb-device=XYZ0123456789

# Notes on Android SDK versions and API levels

## Introduction

This document tries to clarify why particular versions of the Android SDK and
API levels have been chosen for Ouinet, also to guide future version updates
if the need arises.

Please look for occurrences of the name of this document in source code to
find where SDK versions and API levels are relevant, and remember to add such
mentions to this document in new such code.

## Support library dependencies

Ouinet Java code for Android depends on its support libraries.

As of 2021-11-24, it only depends on [annotations][], as indicated in
[ouinet/build.gradle](../android/ouinet/build.gradle):

```
dependencies { // ...
    implementation 'com.android.support:support-annotations:26.1.0' // ...
}
```

The particular version is dictated by the Android Gradle Plugin (AGP)
indicated in [build.gradle](../android/build.gradle):

```
buildscript { // ...
    dependencies { // ...
        classpath 'com.android.tools.build:gradle:3.4.0' // ...
    } // ...
}
```

[annotations]: https://developer.android.com/topic/libraries/support-library/packages#annotations

The AGP's lint tasks force the target SDK to be at least 26 for code to be
accepted in Google Play (when it was released):

> Google Play requires that apps target API level 26 or higher.

To lower that SDK, older versions of the AGP (e.g. 3.0) and annotation library
may be tested, while keeping the same version of Gradle (5.1.1, as older ones
have issues with Java 11 as per [Dockerfile.android](../Dockerfile.android)).

## Compile SDK

According to <https://stackoverflow.com/a/50697767>:

> Your *compile SDK version* **MUST** match the *support library*.

So with the support library having SDK 26 (see above), then in
[ouinet/build.gradle](../android/ouinet/build.gradle):

```
android { // ...
    compileSdkVersion 26 // ...
}
```

## Build SDK

In spite of <https://stackoverflow.com/a/50697767>, AGP 3.4.0 requires build
tools >= 28.0.3, and it ignores whatever Gradle setting for
`buildToolsVersion`.

## Target SDK

According to <https://stackoverflow.com/a/26694276> and
<https://stackoverflow.com/a/37843784>:

> minSdkVersion (lowest possible) <= targetSdkVersion <= (ideally ==) compileSdkVersion

Thus in [ouinet/build.gradle](../android/ouinet/build.gradle):

```
android { // ...
    defaultConfig { // ...
        targetSdkVersion 26 // ...
    } // ...
}
```

## Minimum SDK

Also according to <https://stackoverflow.com/a/41079462>:

> Every time you increase the `APP_PLATFORM` of your project, you have to set
> `minSdkVersion` equal to `APP_PLATFORM`, **preventing your app from running
> on older devices**.

But according to [Application.mk][] (under `APP_PLATFORM`):

> When using Gradle and `externalNativeBuild`, this parameter should not be
> set directly. Instead, set the `minSdkVersion` property in the
> `defaultConfig` or `productFlavors` blocks [...]

[Application.mk]: https://developer.android.com/ndk/guides/application_mk

So setting `minSdkVersion` should suffice.  Factors helping decide the minimum
SDK version:

- The oldest platform supported by `android-ndk-r19b`, as installed by
  `build-android.sh`, is `android-16`.
  However, no application that implements Ouinet supports an SDK lower than 21,
  (e.g. see Ceno Browser [buildSrc/src/main/java/Config.kt](https://gitlab.com/censorship-no/ceno-browser/-/blob/main/buildSrc/src/main/java/Config.kt#L13)).

Thus:

```
android { // ...
    defaultConfig { // ...
        minSdkVersion 21 // ...
    } // ...
}
```

This may be lowered by using an older NDK, if it includes older platforms.
However it may pose problems with applications having a higher minimum SDK.

## Changes for 64-bit builds

Since support for 64-bit architectures was added in Android SDK/API 21, 64-bit
builds raise the minimum SDK version to 21.  `build-android.sh` takes care of
setting the appropriate value of `OUINET_MIN_API` in this case.

The same will happen for builds with targets compiled for different
architectures in the same AAR, like `omni` that includes `armeabi-v7a`,
`arm64-v8a` and `x86_64`). In this case, `build-android.sh` will adjust
`OUINET_MIN_API` to 21 in order to keep compatibility with 64-bit libs.

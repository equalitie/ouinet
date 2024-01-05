### Integrating the Ouinet library into your app

In order for your Android app to access the resources it needs using the HTTP
protocol over Ouinet, thus taking advantage of its caching and distributed
request handling, you need to take few simple steps.

Here we assume that the app is developed in the Android Studio environment,
and that `<PROJECT DIR>` is your app's project directory.

#### Option A: Get Ouinet from Maven Central

Select the Ouinet version according to your app's ABI (we officially support
`ouinet-armeabi-v7a`, `ouinet-arm64-v8a` and `omni` that includes all the
supported ABIs plus `x86_64`), and also add Relinker as adependency in
`<PROJECT DIR>/app/build.gradle`:

```groovy
dependencies {
    //...
    implementation 'ie.equalit.ouinet:ouinet-armeabi-v7a:0.20.0'
    implementation 'com.getkeepsafe.relinker:relinker:1.4.4'
}
```

Check that Maven Central is added to the list of repositories used by
Gradle:

```groovy
allprojects {
    repositories {
        // ...
        mavenCentral()
    }
}
```

Now the Ouinet library will be automatically fetched by Gradle when your app is built.

#### Option B: Use your own compiled version of Ouinet

First, you need to compile the Ouinet library for the ABI environment you are
aiming at (e.g. `armeabi-v7a` or `x86_64`) as described above.  After the
`build_android.sh` script finishes successfully, you can copy the
`ouinet-debug.aar` file to your app libs folder:

```sh
$ cp /path/to/ouinet-debug.aar <PROJECT DIR>/app/libs/
```

Then look for the following section of your `<PROJECT DIR>/build.gradle`:

```groovy
allprojects {
  repositories {
    // ...
  }
}
```

And add this:

```groovy
flatDir {
  dirs 'libs'
}
mavenCentral()  // for ReLinker
```

Then look for the following section of your `<PROJECT DIR>/app/build.gradle`:

```groovy
dependencies {
  // ...
}
```

And add these:

```groovy
implementation 'com.getkeepsafe.relinker:relinker:1.4.4'
implementation(name:'ouinet-debug', ext:'aar')
```

#### Initialize Ouinet

At this stage your project should compile with no errors.  Now to tell Ouinet
to take over the app's HTTP communications, in the `MainActivity.java` of your
app import Ouinet:

```java
import ie.equalit.ouinet.Ouinet;
```

Then add a private member to your `MainActivity` class:

```java
private Ouinet ouinet;
```

And in its `OnCreate` method initiate the Ouinet object (using the BEP5/HTTP
cache):

```java
Config config = new Config.ConfigBuilder(this)
            .setCacheType("bep5-http")
            .setCacheHttpPubKey(<CACHE_PUB_KEY>)
            .setInjectorCredentials(<INJECTOR_USERNAME>:<INJECTOR_PASSWORD>)
            .setInjectorTlsCert(<INJECTOR_TLS_CERT>)
            .setTlsCaCertStorePath(<TLS_CA_CERT_STORE_PATH>)
            .build()

ouinet = new Ouinet(this, config);
ouinet.start();
```

From now on, all of the app's HTTP communication will be handled by Ouinet.

Please note that if you plan to use a directory for Ouinet's static cache in
your application (by using `ConfigBuilder`'s `setCacheStaticPath()` and
`setCacheStaticContentPath()`), then besides the permissions declared by the
library in its manifest, your app will need the `READ_EXTERNAL_STORAGE`
permission (Ouinet will not attempt to write to that directory).

#### Integration Examples

You can find additional information and samples of Android applications using
Ouinet in the following repository:
[equalitie/ouinet-examples](https://gitlab.com/equalitie/ouinet-examples).

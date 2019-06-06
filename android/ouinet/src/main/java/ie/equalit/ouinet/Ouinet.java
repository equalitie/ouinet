package ie.equalit.ouinet;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

public class Ouinet {
    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("ipfs_bindings");
        System.loadLibrary("client");
        System.loadLibrary("native-lib");

        System.setProperty("http.proxyHost", "127.0.0.1");
        System.setProperty("http.proxyPort", "8080");

        System.setProperty("https.proxyHost", "127.0.0.1");
        System.setProperty("https.proxyPort", "8080");
    }
    private static final String TAG = "Ouinet";

    private final Context ctx;
    private final Config config;
    private WifiManager.MulticastLock _lock = null;

    public Ouinet(Context ctx, Config config) {
        this.ctx = ctx;
        this.config = config;
    }

    public void start() {
        try {
            // Just touch this file, as the client looks into the
            // repository and fails if this conf file isn't there.
            new File(config.getOuinetDirectory() + "/ouinet-client.conf").createNewFile();
        } catch (IOException e) {
            Log.d(TAG, "Exception thrown while creating ouinet config file: ", e);
        }

        List<String> args = new ArrayList<>();

        args.add("ouinet-client"); // App name
        args.add("--repo=" + config.getOuinetDirectory());
        args.add("--listen-on-tcp=127.0.0.1:8080");
        args.add("--front-end-ep=127.0.0.1:8081");

        // Useful for debugging
        //args.add("--disable-origin-access");
        //args.add("--disable-proxy-access");
        //args.add("--disable-injector-access");

        maybeAdd(args, "--injector-ep",            config.getInjectorEndpoint());
        maybeAdd(args, "--injector-credentials",   config.getInjectorCredentials());
        maybeAdd(args, "--index-bep44-public-key", config.getIndexBep44PubKey());
        maybeAdd(args, "--index-ipns-id",          config.getIndexIpnsId());
        maybeAdd(args, "--tls-ca-cert-store-path", config.getTlsCaCertStorePath());
        maybeAdd(args, "--injector-tls-cert-file", config.getInjectorTlsCertPath());

        List<String> path = new ArrayList<>();
        if (config.getObfs4ProxyPath() != null) {
            path.add(config.getObfs4ProxyPath());
        }

        nStartClient(args.toArray(new String[0]), path.toArray(new String[0]));
    }

    // If this succeeds, we should be able to do UDP multicasts
    // from inside ouinet (currently know to be needed by IPFS' mDNS
    // but that's not essential for WAN).
    public boolean acquireMulticastLock()
    {
        WifiManager wifi = (WifiManager) ctx.getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);

        if (wifi == null){
            return false;
        }

        _lock = wifi.createMulticastLock("mylock");

        try {
            _lock.acquire();
        } catch (Exception e) {
            _lock = null;
            return false;
        }

        return true;
    }

    // Stop the internal ouinet/client threads. Once this function returns, the
    // ouinet/client will have all of it's resources freed. It should be called
    // no later than in Activity.onDestroy()
    public void stop() {
        nStopClient();
        if (_lock != null) { _lock.release(); }
    }

    // Set injector's IPNS (A.k.a. it's IPFS ID)
    public void setIPNS(String ipns) {
        nSetIPNS(ipns);
    }

    // Set injector endpoint. Can be either in the form IP:PORT or it can be an
    // I2P address.
    public void setInjectorEndpoint(String endpoint) {
        nSetInjectorEP(endpoint);
    }

    public void setCredentialsFor(String injector, String credentials) {
        nSetCredentialsFor(injector, credentials);
    }

    /**
     * Wrapper for the native method as native method cannot be mocked in tests.
     */
    static String getCARootCert(String ouinetDirectory) {
        return nGetCARootCert(ouinetDirectory);
    }


    private void maybeAdd(List<String> args, String key, String value) {
        if (value == null || value.isEmpty()) return;
        args.add(key + "=" + value);
    }

    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */

    /**
     * Returns the path to the CA root certificate. If the certificate does not exist it will be
     * created.
     * @return Path to CA root certificate.
     */
    private static native String nGetCARootCert(String ouinetDirectory);
    private native void nStartClient(String[] args, String[] path);
    private native void nStopClient();
    private native void nSetInjectorEP(String endpoint);
    private native void nSetCredentialsFor(String injector, String cred);
    private native void nSetIPNS(String ipns);
}

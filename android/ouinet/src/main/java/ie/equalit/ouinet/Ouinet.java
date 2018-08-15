package ie.equalit.ouinet;

import android.content.Context;

import android.net.wifi.WifiManager;
import java.io.File;

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

    private Context _ctx;
    private WifiManager.MulticastLock _lock = null;

    public Ouinet(Context ctx, String ipns,
                               String injector_ep,
                               String credentials) {
        _ctx = ctx;

        if (ipns        == null) ipns        = "";
        if (injector_ep == null) injector_ep = "";
        if (credentials == null) credentials = "";

        new File(dir()).mkdirs();

        nStartClient(dir(),
                injector_ep,
                ipns,
                credentials,
                false);
    }

    // If this succeeds, we should be able to do UDP multicasts
    // from inside ouinet (currently know to be needed by IPFS' mDNS
    // but that's not essential for WAN).
    public boolean acquireMulticastLock()
    {
        WifiManager wifi = (WifiManager) _ctx.getSystemService(Context.WIFI_SERVICE);

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

    //----------------------------------------------------------------
    private String dir() {
        return _ctx.getFilesDir().getAbsolutePath() + "/ouinet";
    }

    //----------------------------------------------------------------
    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    private native void nStartClient( String repo_root
                                    , String injector
                                    , String ipns
                                    , String credentials
                                    , boolean enable_http_connect_requests);

    private native void nStopClient();
    private native void nSetInjectorEP(String endpoint);
    private native void nSetCredentialsFor(String injector, String cred);
    private native void nSetIPNS(String ipns);
}

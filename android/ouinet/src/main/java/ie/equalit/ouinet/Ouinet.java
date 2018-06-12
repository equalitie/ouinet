package ie.equalit.ouinet;

import android.content.Context;

import android.net.wifi.WifiManager;

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

    public Ouinet(Context ctx) {
        _ctx = ctx;

        String ipns        = readIPNS();
        String injector_ep = readInjectorEP();
        String credentials = readCredentialsFor(injector_ep);

        WifiManager wifi = (WifiManager)ctx.getSystemService(Context.WIFI_SERVICE);
        if (wifi != null){
            _lock = wifi.createMulticastLock("mylock");
            _lock.acquire();
        }

        nStartClient(_ctx.getFilesDir().getAbsolutePath(),
                injector_ep,
                ipns,
                credentials,
                false);
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
        writeIPNS(ipns);
        nSetIPNS(ipns);
    }

    // Get injector's IPNS (A.k.a. it's IPFS ID)
    public String getIPNS() {
        return readIPNS();
    }

    // Set injector endpoint. Can be either in the form IP:PORT or it can be an
    // I2P address.
    public void setInjectorEndpoint(String endpoint) {
        writeInjectorEP(endpoint);
        nSetInjectorEP(endpoint);
    }

    // Return injector's endpoint as specified with the setInjectorEndpoint
    // function.
    public String getInjectorEndpoint() {
        return readInjectorEP();
    }

    public void setCredentialsFor(String injector, String credentials) {
        writeCredentials(injector, credentials);
        nSetCredentialsFor(injector, credentials);
    }

    public String getCredentialsFor(String injector) {
        return readCredentialsFor(injector);
    }

    //----------------------------------------------------------------
    protected String dir() { return _ctx.getFilesDir().getAbsolutePath(); }

    protected String config_ipns()        { return dir() + "/ipns.txt";        }
    protected String config_injector()    { return dir() + "/injector.txt";    }
    protected String config_credentials() { return dir() + "/credentials.txt"; }

    protected void writeIPNS(String s)       { Util.saveToFile(_ctx, config_ipns(), s); }
    protected void writeInjectorEP(String s) { Util.saveToFile(_ctx, config_injector(), s); }

    protected String readIPNS()       { return Util.readFromFile(_ctx, config_ipns(), ""); }
    protected String readInjectorEP() { return Util.readFromFile(_ctx, config_injector(), ""); }

    protected void writeCredentials(String injector, String cred) {
        Util.saveToFile(_ctx, config_credentials(), injector + "\n" + cred);
    }

    protected String readCredentialsFor(String injector) {
        if (injector == null || injector.length() == 0) return "";

        String content = Util.readFromFile(_ctx, config_credentials(), null);

        if (content == null) { return ""; }

        String[] lines = content.split("\\n");

        if (lines.length != 2)          { return ""; }
        if (!lines[0].equals(injector)) { return ""; }

        return lines[1];
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

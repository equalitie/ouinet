package ie.equalit.ouinet;

import android.content.Context;

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

    public Ouinet(Context ctx) {
        _ctx = ctx;

        String ipns = readIPNS();
        if (ipns.length() > 0) { nSetIPNS(ipns); }

        String injector_ep = readInjectorEP();
        if (injector_ep.length() > 0) { setInjectorEP(injector_ep); }

        nStartClient(_ctx.getFilesDir().getAbsolutePath(), injector_ep, ipns);
    }

    public void stop() {
        nStopClient();
    }

    public void setInjectorEP(String endpoint) {
        writeInjectorEP(endpoint);
        nSetInjectorEP(endpoint);
    }

    public void setIPNS(String ipns) {
        writeIPNS(ipns);
        nSetIPNS(ipns);
    }

    public String getIPNS() {
        return readIPNS();
    }

    public String getInjectorEndpoint() {
        return readInjectorEP();
    }

    //----------------------------------------------------------------
    protected String dir() { return _ctx.getFilesDir().getAbsolutePath(); }

    protected void writeIPNS(String s)       { Util.saveToFile(_ctx, dir()+"ipns.txt", s); }
    protected void writeInjectorEP(String s) { Util.saveToFile(_ctx, dir()+"injector.txt", s); }

    protected String readIPNS()       { return Util.readFromFile(_ctx, dir()+"ipns.txt", ""); }
    protected String readInjectorEP() { return Util.readFromFile(_ctx, dir()+"injector.txt", ""); }


    //----------------------------------------------------------------
    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    private native void nStartClient(String repo_root, String injector, String ipns);
    private native void nStopClient();
    private native void nSetInjectorEP(String endpoint);
    private native void nSetIPNS(String ipns);
}

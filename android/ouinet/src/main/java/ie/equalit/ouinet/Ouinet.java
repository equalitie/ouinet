package ie.equalit.ouinet;

import android.content.Context;
import android.net.wifi.WifiManager;

import java.io.File;
import java.io.IOException;
import java.io.FileOutputStream;

import java.util.*;
import android.util.Log;

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

    public static class Config {
        public String injector_ipfs_id;
        public String injector_endpoint;
        public String injector_credentials;
        public String injector_tls_cert;
        // Path to a .pem file with CA certificates.
        // One can get it from e.g.: https://curl.haxx.se/docs/caextract.html
        public String tls_ca_cert_store_path;
    }

    public Ouinet(Context ctx, Config conf) {
        _ctx = ctx;

        Vector<String> args = new Vector<String>();

        new File(dir()).mkdirs();

        try {
            // Just touch this file, as the client looks into the
            // repository and fails if this conf file isn't there.
            new File(dir() + "/ouinet-client.conf").createNewFile();
        } catch (IOException e) {
            Log.d("Ouinet",
                    "Exception thrown while creating ouinet config file: " + e);
        }

        args.addElement("ouinet-client"); // App name
        args.addElement("--repo=" + dir());
        args.addElement("--listen-on-tcp=127.0.0.1:8080");
        args.addElement("--front-end-ep=0.0.0.0:8081");

        // Temporary while the app is being tested.
        args.addElement("--disable-origin-access");

        maybeAdd(args, "--injector-ep",          conf.injector_endpoint);
        maybeAdd(args, "--injector-ipns",        conf.injector_ipfs_id);
        maybeAdd(args, "--injector-credentials", conf.injector_credentials);

        if (conf.tls_ca_cert_store_path != null) {
            String ca_cert_path;
            String assetPrefix = "file:///android_asset/";

            if (conf.tls_ca_cert_store_path.startsWith(assetPrefix)) {
                // TODO: Ouinet's C++ code doesn't yet have a way to read asset
                // files from the apk. As a temporary workaround we copy the
                // asset file to a regular file and the pass path to that to
                // the C++ code.
                String asset = conf.tls_ca_cert_store_path.substring(assetPrefix.length());
                ca_cert_path = dir() + "/assets/" + asset;

                if (copyAssetToFile(ctx, asset, ca_cert_path)) {
                    maybeAdd(args, "--tls-ca-cert-store-path", ca_cert_path);
                }
            }
            else {
                maybeAdd(args, "--tls-ca-cert-store-path", conf.tls_ca_cert_store_path);
            }
        }

        try {
            if (conf.injector_tls_cert != null) {
                String cert_path = dir() + "/injector-tls-cert.pem";
                writeToFile(cert_path, conf.injector_tls_cert.getBytes());
                args.addElement("--injector-tls-cert-file=" + cert_path);
            }
        } catch (IOException e) {
            Log.d("Ouinet",
                    "Exception thrown while creating injector's cert file: " + e);
            e.printStackTrace();
        }

        nStartClient(listToArray(args));
    }

    public String pathToCARootCert()
    {
        return nPathToCARootCert();
    }

    public boolean copyAssetToFile(Context ctx, String asset, String path)
    {
        try {
            java.io.InputStream stream = ctx.getAssets().open(asset);
            int size = stream.available();
            byte[] buffer = new byte[size];
            stream.read(buffer);
            stream.close();
            writeToFile(path, buffer);
        } catch (IOException e) {
            Log.d("Ouinet", "Failed to write asset \"" + asset + "\" to file \"" + path + "\"");
            e.printStackTrace();
            return false;
        }

        return true;
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
    private String[] listToArray(List<String> list) {
        String[] ret = new String[list.size()];
        int i = 0;
        for (String s : list) { ret[i] = s; i += 1; }
        return ret;
    }

    private void maybeAdd(Vector<String> args, String key, String value) {
        if (value == null) return;
        args.addElement(key + "=" + value);
    }

    private void writeToFile(String path, byte[] bytes) throws IOException {
        File file = new File(path);

        if (!file.exists()) {
            File dir = file.getParentFile();
            if (!dir.exists()) { dir.mkdirs(); }
            file.createNewFile();
        }

        FileOutputStream stream = new FileOutputStream(file);
        stream.write(bytes);
        stream.close();
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
    private native void nStartClient(String[] args);

    private native void nStopClient();
    private native void nSetInjectorEP(String endpoint);
    private native void nSetCredentialsFor(String injector, String cred);
    private native void nSetIPNS(String ipns);
    private native String nPathToCARootCert();
}

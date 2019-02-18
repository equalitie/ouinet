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
    private static final String TAG = "Ouinet";
    private Context _ctx;
    private WifiManager.MulticastLock _lock = null;

    public static class Config {
        public String index_bep44_pubkey;
        public String index_ipns_id;
        public String injector_endpoint;
        public String injector_credentials;
        public String injector_tls_cert;
        // Path to a .pem file with CA certificates.
        // One can get it from e.g.: https://curl.haxx.se/docs/caextract.html
        public String tls_ca_cert_store_path;
    }

    public Ouinet(Context ctx, Config conf) {
        _ctx = ctx;

        List<String> args = new ArrayList<String>();
        List<String> path = new ArrayList<String>();


        new File(dir()).mkdirs();

        try {
            // Just touch this file, as the client looks into the
            // repository and fails if this conf file isn't there.
            new File(dir() + "/ouinet-client.conf").createNewFile();
        } catch (IOException e) {
            Log.d(TAG, "Exception thrown while creating ouinet config file: ", e);
        }

        args.add("ouinet-client"); // App name
        args.add("--repo=" + dir());
        args.add("--listen-on-tcp=127.0.0.1:8080");
        args.add("--front-end-ep=0.0.0.0:8081");

        maybeAdd(args, "--injector-ep",            conf.injector_endpoint);
        maybeAdd(args, "--injector-credentials",   conf.injector_credentials);
        maybeAdd(args, "--index-bep44-public-key", conf.index_bep44_pubkey);
        maybeAdd(args, "--index-ipns-id",          conf.index_ipns_id);

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

                if (copyAssetToFile(asset, ca_cert_path)) {
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
                args.add("--injector-tls-cert-file=" + cert_path);
            }
        } catch (IOException e) {
            Log.d(TAG, "Exception thrown while creating injector's cert file: ", e);
        }

        String objfs4proxy_path = dir() + "/objfs4proxy";
        if (copyExecutableToFile("obfs4proxy", objfs4proxy_path)) {
            Log.d(TAG, "objfs4proxy copied to " + objfs4proxy_path);
            path.add(dir());
        } else {
            Log.d(TAG, "objfs4proxy not copied");
        }

        nStartClient(args.toArray(new String[0]), path.toArray(new String[0]));
    }

    public String pathToCARootCert()
    {
        return nPathToCARootCert();
    }

    public boolean copyAssetToFile(String asset, String path)
    {
        try {
            java.io.InputStream stream = _ctx.getAssets().open(asset);
            int size = stream.available();
            byte[] buffer = new byte[size];
            stream.read(buffer);
            stream.close();
            writeToFile(path, buffer);
        } catch (IOException e) {
            Log.d(TAG, "Failed to write asset \"" + asset + "\" to file \"" + path + "\"", e);
            return false;
        }

        return true;
    }

    public boolean copyExecutableToFile(String asset, String path) {
        if (!copyAssetToFile(asset, path)) {
            return false;
        }
        File executable = new File(path);
        if (!executable.setExecutable(true)) {
            Log.d(TAG, "Failed to set executable for file: " + path);
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
    private void maybeAdd(List<String> args, String key, String value) {
        if (value == null || value.isEmpty()) return;
        args.add(key + "=" + value);
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
    private native void nStartClient(String[] args, String[] path);

    private native void nStopClient();
    private native void nSetInjectorEP(String endpoint);
    private native void nSetCredentialsFor(String injector, String cred);
    private native void nSetIPNS(String ipns);
    private native String nPathToCARootCert();
}

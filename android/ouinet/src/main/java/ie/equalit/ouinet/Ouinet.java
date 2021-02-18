package ie.equalit.ouinet;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.net.wifi.WifiManager;
import android.support.annotation.Nullable;
import android.support.annotation.VisibleForTesting;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

public class Ouinet {
    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("client");
        System.loadLibrary("native-lib");

        System.setProperty("http.proxyHost", "127.0.0.1");
        System.setProperty("http.proxyPort", "8077");

        System.setProperty("https.proxyHost", "127.0.0.1");
        System.setProperty("https.proxyPort", "8077");
    }

    private static final String TAG = "OuinetJava";

    private final Context context;
    private final Config config;
    private BroadcastReceiver wifiChangeReceiver;
    private BroadcastReceiver chargingChangeReceiver;
    private WifiManager.MulticastLock lock;

    public Ouinet(Context context, Config config) {
        this.context = context;
        this.config = config;
    }

    public synchronized void start() {
        try {
            // Just touch this file, as the client looks into the
            // repository and fails if this conf file isn't there.
            new File(config.getOuinetDirectory() + "/ouinet-client.conf").createNewFile();
        } catch (IOException e) {
            Log.d(TAG, "Exception thrown while creating ouinet config file: ", e);
        }

        if (!acquireMulticastLock()) {
            Log.d(TAG, "Failed to acquire multicast lock");
        }

        List<String> args = new ArrayList<>();
        args.add("ouinet-client"); // App name
        args.add("--repo=" + config.getOuinetDirectory());

        // If default client endpoints clash with other ports,
        // uncomment these and change `http(s).proxyPort` above to match.
        //args.add("--listen-on-tcp=127.0.0.1:8177");
        //args.add("--front-end-ep=127.0.0.1:8178");

        // Useful for debugging
        //args.add("--disable-origin-access");
        //args.add("--disable-proxy-access");
        //args.add("--disable-injector-access");

        maybeAdd(args, "--injector-credentials",   config.getInjectorCredentials());
        maybeAdd(args, "--cache-http-public-key",  config.getCacheHttpPubKey());
        maybeAdd(args, "--tls-ca-cert-store-path", config.getTlsCaCertStorePath());
        maybeAdd(args, "--injector-tls-cert-file", config.getInjectorTlsCertPath());
        maybeAdd(args, "--cache-type",             config.getCacheType());

        List<String> path = new ArrayList<>();
        if (config.getObfs4ProxyPath() != null) {
            path.add(config.getObfs4ProxyPath());
        }

        nStartClient(args.toArray(new String[0]), path.toArray(new String[0]));

        registerBroadcastReceivers();
    }

    // If this succeeds, we should be able to do UDP multicasts
    // from inside ouinet (currently know to be needed by IPFS' mDNS
    // but that's not essential for WAN).
    public boolean acquireMulticastLock() {
        WifiManager wifi = (WifiManager) context.getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);
        if (wifi == null) {
            return false;
        }

        lock = wifi.createMulticastLock("ouinet.multicast.lock");

        try {
            lock.acquire();
        } catch (Exception e) {
            lock = null;
            return false;
        }
        return true;
    }

    // Stop the internal ouinet/client threads. Once this function returns, the
    // ouinet/client will have all of it's resources freed. It should be called
    // no later than in Activity.onDestroy()
    public synchronized void stop() {
        nStopClient();
        if (lock != null) {
            lock.release();
        }
        if (wifiChangeReceiver != null) {
            context.unregisterReceiver(wifiChangeReceiver);
            wifiChangeReceiver = null;
        }
        if (chargingChangeReceiver != null) {
            context.unregisterReceiver(chargingChangeReceiver);
            chargingChangeReceiver = null;
        }
    }

    public void setCredentialsFor(String injector, String credentials) {
        nSetCredentialsFor(injector, credentials);
    }

    private void registerBroadcastReceivers() {
        wifiChangeReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String action = intent.getAction();
                if (WifiManager.NETWORK_STATE_CHANGED_ACTION.equals(action)) {
                    NetworkInfo info =
                            intent.getParcelableExtra(WifiManager.EXTRA_NETWORK_INFO);
                    if (info == null) {
                        return;
                    }
                    if (info.isConnected()) {
                        nWifiStateChange(true);
                    }
                } else if (ConnectivityManager.CONNECTIVITY_ACTION.equals(action)) {
                    NetworkInfo info =
                            intent.getParcelableExtra(ConnectivityManager.EXTRA_NETWORK_INFO);
                    if (info == null) {
                        return;
                    }
                    if (info.getType() == ConnectivityManager.TYPE_WIFI && !info.isConnected()) {
                        nWifiStateChange(false);
                    }
                }
            }
        };
        IntentFilter wifiIntentFilter = new IntentFilter();
        wifiIntentFilter.addAction(WifiManager.NETWORK_STATE_CHANGED_ACTION);
        wifiIntentFilter.addAction(ConnectivityManager.CONNECTIVITY_ACTION);
        context.registerReceiver(wifiChangeReceiver, wifiIntentFilter);

        chargingChangeReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                boolean isCharging = Intent.ACTION_POWER_CONNECTED.equals(intent.getAction());
                nChargingStateChange(isCharging);
            }
        };
        IntentFilter powerIntentFilter = new IntentFilter();
        powerIntentFilter.addAction(Intent.ACTION_POWER_CONNECTED);
        powerIntentFilter.addAction(Intent.ACTION_POWER_DISCONNECTED);
        context.registerReceiver(chargingChangeReceiver, powerIntentFilter);
    }

    /**
     * Wrapper for the native method as native methods cannot be mocked in tests.
     */
    static String getCARootCert(String ouinetDirectory) {
        return nGetCARootCert(ouinetDirectory);
    }

    private void maybeAdd(List<String> args, String key, @Nullable String value) {
        if (value == null || value.isEmpty()) return;
        args.add(key + "=" + value);
    }

    /*
     * Native methods that are implemented by the 'native-lib' native library,
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
    private native void nSetCredentialsFor(String injector, String cred);
    private native void nChargingStateChange(boolean isCharging);
    private native void nWifiStateChange(boolean isWifiConnected);
}

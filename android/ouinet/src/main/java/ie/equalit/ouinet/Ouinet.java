package ie.equalit.ouinet;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.support.annotation.Nullable;
import android.support.annotation.VisibleForTesting;
import android.util.Log;

import com.getkeepsafe.relinker.ReLinker;
import com.getkeepsafe.relinker.ReLinkerInstance;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

public class Ouinet {
    private static boolean libsLoaded = false;

    // Used to load the 'native-lib' library on application startup.
    public static synchronized void maybeLoadLibraries(Context context) {
        if (libsLoaded) return;

        // Explicitly loading library dependencies is needed for older versions of Android
        // (probably 16 <= API < 19).
        // ReLinker should take care of working around these issues,
        // but it still has some problems with API < 18
        // (see <https://github.com/KeepSafe/ReLinker/issues/15>).
        if (Build.VERSION.SDK_INT < 18) {
            System.loadLibrary("c++_shared");
            System.loadLibrary("boost_asio");
            System.loadLibrary("boost_asio_ssl");
            System.loadLibrary("gpg-error");
            System.loadLibrary("gcrypt");

            System.loadLibrary("client");
            System.loadLibrary("native-lib");
        } else {
            ReLinkerInstance relinker = ReLinker.recursively();
            relinker.loadLibrary(context, "client");
            relinker.loadLibrary(context, "native-lib");
        }

        libsLoaded = true;
    }

    // Since the client can be started several times,
    // there are no real "initial" and "final" states.
    // The client should be in Created or Stopped state before starting it;
    // if a short time after start it is in the Created, Failed or Stopped states,
    // its start can be considered as failed.
    public enum RunningState {
        Created,  // not told to start yet (initial)
        Failed,  // told to start, error precludes from continuing (final)
        Starting,  // told to start, some operations still pending completion
        Degraded,  // told to start, some operations succeeded but others failed
        Started,  // told to start, all operations succeeded
        Stopping,  // told to stop, some operations still pending completion
        Stopped,  // told to stop, all operations succeeded (final)
    }

    private static final String TAG = "OuinetJava";

    private final Context context;
    private final Config config;
    private BroadcastReceiver wifiChangeReceiver;
    private BroadcastReceiver chargingChangeReceiver;
    private WifiManager.MulticastLock lock;

    public Ouinet(Context context, Config config) {
        maybeLoadLibraries(context);

        this.context = context;
        this.config = config;
    }

    // See the comments about RunningState above for
    // the meaning of the different states returned.
    public RunningState getState() {
        // TODO: Avoid needing to keep this in sync by hand.
        switch (nGetClientState()) {
        case 0: return RunningState.Created;
        case 1: return RunningState.Failed;
        case 2: return RunningState.Starting;
        case 3: return RunningState.Degraded;
        case 4: return RunningState.Started;
        case 5: return RunningState.Stopping;
        case 6: return RunningState.Stopped;
        }
        return RunningState.Failed;
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

        List<String> args = new ArrayList<String>();
        args.add("ouinet-client"); // App name
        args.add("--repo=" + config.getOuinetDirectory());

        // If default client endpoints clash with other ports,
        // use setListenOnTcp or setFrontEndEp in the Config builder
        // and change `http(s).proxyPort` to match.
        maybeAdd(args, "--listen-on-tcp",          config.getListenOnTcp());
        maybeAdd(args, "--front-end-ep",           config.getFrontEndEp());

        maybeAdd(args, "--injector-credentials",   config.getInjectorCredentials());
        maybeAdd(args, "--cache-http-public-key",  config.getCacheHttpPubKey());
        maybeAdd(args, "--tls-ca-cert-store-path", config.getTlsCaCertStorePath());
        maybeAdd(args, "--injector-tls-cert-file", config.getInjectorTlsCertPath());
        maybeAdd(args, "--cache-type",             config.getCacheType());
        maybeAdd(args, "--cache-static-repo",      config.getCacheStaticPath());
        maybeAdd(args, "--cache-static-root",      config.getCacheStaticContentPath());

        if (config.getLogLevel() != null) {
            args.add("--log-level=" + config.getLogLevel().name());
        }

        if (config.getEnableLogFile()) {
            args.add("--enable-log-file");
        }

        if (config.getDisableOriginAccess()) {
            args.add("--disable-origin-access");
        }

        if (config.getDisableProxyAccess()) {
            args.add("--disable-proxy-access");
        }

        if (config.getDisableInjectorAccess()) {
            args.add("--disable-injector-access");
        }

        if (config.getCachePrivate()) {
            args.add("--cache-private");
        }

        List<String> path = new ArrayList<String>();
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
    private native int nGetClientState();
    private native void nStartClient(String[] args, String[] path);
    private native void nStopClient();
    private native void nChargingStateChange(boolean isCharging);
    private native void nWifiStateChange(boolean isWifiConnected);
}

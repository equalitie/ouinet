package ie.equalit.ouinet_examples.android_java;

import android.util.Log;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;
import android.view.View;
import androidx.appcompat.app.AppCompatActivity;
import android.os.Bundle;
import ie.equalit.ouinet.Config;
import ie.equalit.ouinet.Ouinet;
import okhttp3.*;

import javax.net.ssl.*;
import java.io.*;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.URI;
import java.net.URISyntaxException;
import java.security.KeyManagementException;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.NoSuchAlgorithmException;
import java.security.cert.Certificate;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.concurrent.Executors;

public class MainActivity extends AppCompatActivity {
    private Ouinet ouinet;
    private String ouinet_dir;
    private static final String TAG = "OuinetTester";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        Config config = new Config.ConfigBuilder(this)
                .setCacheType("bep5-http")
                .setTlsCaCertStorePath("file:///android_asset/cacert.pem")
                .setCacheHttpPubKey(BuildConfig.CACHE_PUB_KEY)
                .setInjectorCredentials(BuildConfig.INJECTOR_CREDENTIALS)
                .setInjectorTlsCert(BuildConfig.INJECTOR_TLS_CERT)
                .setLogLevel(Config.LogLevel.DEBUG)
                .setListenOnTcp("127.0.0.1:8888")
                .setFrontEndEp("127.0.0.1:5555")
                //.setDisableOriginAccess(true)
                //.setDisableProxyAccess(true)
                //.setDisableInjectorAccess(true)
                .build();
        ouinet_dir = config.getOuinetDirectory();

        ouinet = new Ouinet(this, config);
        ouinet.start();

        Executors.newFixedThreadPool(1).execute((Runnable) this::updateServiceState);
    }

    private void updateServiceState() {
      TextView ouinetState = (TextView) findViewById(R.id.ouinetStatus);

      while(true)
      {
          try {
              Thread.sleep(1000);
          } catch (InterruptedException e) {
              e.printStackTrace();
          }

          String state = ouinet.getState().toString();
          runOnUiThread(() -> ouinetState.setText("State: " + state));
      }
    }

    public void getURL(View view) {
        EditText editText = (EditText) findViewById(R.id.url);
        TextView logViewer = (TextView) findViewById(R.id.log_viewer);
        String url = editText.getText().toString();

        Toast toast = Toast.makeText(this, "Loading: " + url, Toast.LENGTH_SHORT);
        toast.show();

        OkHttpClient client = getOuinetHttpClient();
        Request request = new Request.Builder()
                .url(url)
                .header("X-Ouinet-Group", getDhtGroup(url))
                .build();

        client.newCall(request).enqueue(new Callback() {
            public void onFailure(Call call, IOException e) {
                e.printStackTrace();
                logViewer.setText(e.toString());
            }

            public void onResponse(Call call, Response response) throws IOException {
                try (ResponseBody responseBody = response.body()) {
                    Headers responseHeaders = response.headers();
                    for (int i = 0, size = responseHeaders.size(); i < size; i++) {
                        System.out.println(responseHeaders.name(i) + ": " + responseHeaders.value(i));
                    }

                    logViewer.setText(responseHeaders.toString());
                }
            }
        });
    }

    private String getDhtGroup(String url) {
        String domain = null;
        try {
            domain = new URI(url).getSchemeSpecificPart();
            domain = domain.replaceAll("^//", "");
        } catch (URISyntaxException e) {
            e.printStackTrace();
        }
        return domain;
    }

    private OkHttpClient getOuinetHttpClient() {
        try {
            TrustManager[] trustManagers = getOuinetTrustManager();

            OkHttpClient.Builder builder = new OkHttpClient.Builder();
            builder.sslSocketFactory(
                    getSSLSocketFactory(trustManagers),
                    (X509TrustManager) trustManagers[0]);

            // Proxy to ouinet service
            Proxy ouinetService= new Proxy(Proxy.Type.HTTP, new InetSocketAddress("127.0.0.1", 8888));
            builder.proxy(ouinetService);
            return builder.build();
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    private SSLSocketFactory getSSLSocketFactory(TrustManager[] trustManagers) throws NoSuchAlgorithmException, KeyManagementException {
        final SSLContext sslContext = SSLContext.getInstance("TLS");
        sslContext.init(null, trustManagers, new java.security.SecureRandom());
        return sslContext.getSocketFactory();
    }

    private TrustManager[] getOuinetTrustManager() throws NoSuchAlgorithmException, KeyStoreException, CertificateException, IOException {
        return new TrustManager[]{new OuinetTrustManager()};
    }

    private class OuinetTrustManager implements X509TrustManager {
        private X509TrustManager trustManager;
        private Certificate ca;

        public OuinetTrustManager() throws NoSuchAlgorithmException, KeyStoreException, CertificateException, IOException {
            TrustManagerFactory tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            tmf.init(getKeyStore());

            for (TrustManager tm : tmf.getTrustManagers()) {
                if (tm instanceof X509TrustManager) {
                    trustManager = (X509TrustManager) tm;
                    break;
                }
            }
        }

        private KeyStore getKeyStore() throws KeyStoreException, CertificateException, NoSuchAlgorithmException, IOException {
            KeyStore keyStore = KeyStore.getInstance(KeyStore.getDefaultType());
            keyStore.load(null, null);
            keyStore.setCertificateEntry("ca", getCertificateAuthority());
            return keyStore;
        }

        private Certificate getCertificateAuthority() throws CertificateException {
            InputStream caInput = null;
            try {
                caInput = new FileInputStream(ouinet_dir + "/ssl-ca-cert.pem");
            } catch (FileNotFoundException e) {
                e.printStackTrace();
            }

            CertificateFactory cf = CertificateFactory.getInstance("X.509");
            ca = cf.generateCertificate(caInput);
            return ca;
        }

        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException {
            for (X509Certificate cert : chain) {
                Log.d(TAG, "Server Cert Issuer: " + cert.getIssuerDN().getName() + " " + cert.getSubjectDN().getName());
            }
            for (X509Certificate cert : trustManager.getAcceptedIssuers()) {
                Log.d(TAG, "Client Trusted Issuer: " + cert.getIssuerDN().getName());
            }

            trustManager.checkServerTrusted(chain, authType);
        }

        @Override
        public X509Certificate[] getAcceptedIssuers() {
            return new X509Certificate[]{(X509Certificate) ca};
        }

    }
}
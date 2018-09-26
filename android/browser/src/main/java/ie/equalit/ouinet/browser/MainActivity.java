package ie.equalit.ouinet.browser;

import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;

import android.view.Menu;
import android.view.MenuItem;

import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.HttpAuthHandler;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.view.View.OnClickListener;
import android.widget.EditText;
import android.text.InputType;

import com.google.zxing.integration.android.IntentResult;
import com.google.zxing.integration.android.IntentIntegrator;

import android.content.Intent;
import android.widget.Toast;

import ie.equalit.ouinet.browser.OuiWebViewClient;
import ie.equalit.ouinet.browser.Util;
import ie.equalit.ouinet.Ouinet;

interface OnInput {
    public void call(String input);
}

public class MainActivity extends AppCompatActivity {

    private WebView _webView;
    private OuiWebViewClient _webViewClient;
    private Ouinet _ouinet;

    void go_home() {
        String home = "http://www.bbc.com";
        Util.log("Requesting " + home);
        _webView.loadUrl(home);
    }

    void reload() {
        Util.log("Reload");
        _webView.reload();
    }

    protected void loadConfigFromQR() {
        // Start the QR config reader intent.
        IntentIntegrator integrator = new IntentIntegrator(this);
        integrator.initiateScan();
    }

    protected void toast(String s) {
        Util.log("Toast: " + s);
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }

    // Parse config string in the form:
    //
    //     ipns=<IPNS-STRING-STARTING-WITH-Qm>
    //     injector=<IP_AND_PORT-OR-I2P_DESTINATION>
    //     credentials=<USERNAME:PASSWORD>
    //
    // Note that the config doesn't have to contain all of the entries, but the
    // 'credentials' entry must be used with the `injector` entry.
    protected void applyConfig(String config) {
        String[] lines = config.split("[\\r?\\n]+");

        String credentials = null;
        String injector = null;

        for (String line:lines) {
            String[] keyval = line.trim().split("\\s*=\\s*", 2);

            if (keyval.length != 2) {
                continue;
            }

            String key = keyval[0];
            String val = keyval[1];

            Util.log("key: " + key + " value: " + val);
            if (key.equalsIgnoreCase("ipns")) {
                toast("Setting IPNS to: " + val);
                writeIPNS(val);
                _ouinet.setIPNS(val);
            }
            else if (key.equalsIgnoreCase("injector")) {
                toast("Setting injector to: " + val);
                writeInjectorEP(val);
                _ouinet.setInjectorEndpoint(val);
                injector = val;
            }
            else if (key.equalsIgnoreCase("credentials")) {
                credentials = val;
            }
            else {
                toast("Unknown config key: " + key);
            }

            if (credentials != null) {
                if (injector == null) {
                    toast("The 'credentials' option not used without injector");
                }
                else {
                    toast("Setting up credentials");
                    writeCredentials(injector, credentials);
                    _ouinet.setCredentialsFor(injector, credentials);
                }
            }
        }
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        IntentResult result = IntentIntegrator.parseActivityResult(requestCode, resultCode, data);
        if (result == null) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }

        if(result.getContents() != null) {
            applyConfig(result.getContents());
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        String ipns        = readIPNS();
        String injector_ep = readInjectorEP();
        String credentials = readCredentialsFor(injector_ep);

        _ouinet = new Ouinet(this, ipns, injector_ep, credentials);

        _ouinet.acquireMulticastLock();

        setContentView(R.layout.activity_main);

        _webView = (WebView) findViewById(R.id.webview);

        WebSettings webSettings = _webView.getSettings();

        // The first two are necessary for BBC to show correctly.
        webSettings.setJavaScriptEnabled(true);
        webSettings.setDomStorageEnabled(true);
        webSettings.setLoadWithOverviewMode(true);
        webSettings.setUseWideViewPort(true);
        webSettings.setBuiltInZoomControls(true);
        webSettings.setDisplayZoomControls(false);
        webSettings.setSupportZoom(true);
        webSettings.setDefaultTextEncodingName("utf-8");

        _webViewClient = new OuiWebViewClient(this, _ouinet);

        _webView.setWebViewClient(_webViewClient);

        go_home();
    }

    protected void showDialog(String title, String default_value, final OnInput onInput) {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle(title);

        final EditText input = new EditText(this);
        input.setText(default_value);
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        builder.setView(input);

        // Set up the buttons
        builder.setPositiveButton("OK", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                onInput.call(input.getText().toString());
            }
        });
        builder.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                dialog.cancel();
            }
        });

        builder.show();
    }

    protected void showChangeInjectorDialog() {
        String ep = readInjectorEP();

        showDialog("Injector endpoint", ep, new OnInput() {
            @Override
            public void call(String input) {
                writeInjectorEP(input);
                _ouinet.setInjectorEndpoint(input);
            }
        });
    }

    protected void showChangeIPNSDialog() {
        showDialog("IPNS", readIPNS(), new OnInput() {
            @Override
            public void call(String input) {
                writeIPNS(input);
                _ouinet.setIPNS(input);
            }
        });
    }

    @Override
    protected void onDestroy() {
        _ouinet.stop();
        super.onDestroy();
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(Menu.NONE, 1, Menu.NONE, "Home");
        menu.add(Menu.NONE, 2, Menu.NONE, "Reload");
        menu.add(Menu.NONE, 3, Menu.NONE, "Toggle frontend");
        menu.add(Menu.NONE, 4, Menu.NONE, "Clear cache");
        menu.add(Menu.NONE, 5, Menu.NONE, "Injector endpoint");
        menu.add(Menu.NONE, 6, Menu.NONE, "IPNS");
        menu.add(Menu.NONE, 7, Menu.NONE, "Load config from QR");
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        switch (item.getItemId()) {
            case 1:
                go_home();
                return true;
            case 2:
                reload();
                return true;
            case 3:
                _webViewClient.toggleShowClientFrontend();
                _webView.clearView();
                _webView.loadUrl("about:blank");
                go_home();
                return true;
            case 4:
                _webView.clearCache(true);
                return true;
            case 5:
                showChangeInjectorDialog();
                return true;
            case 6:
                showChangeIPNSDialog();
                return true;
            case 7:
                loadConfigFromQR();
                return true;
            default:
                return super.onOptionsItemSelected(item);
        }
    }

    @Override
    public void onBackPressed() {
        if(_webView.canGoBack()) {
            _webView.goBack();
        } else {
            super.onBackPressed();
        }
    }

    //----------------------------------------------------------------
    private String dir() {
        return getFilesDir().getAbsolutePath();
    }

    private String config_ipns()        { return dir() + "/ipns.txt";        }
    private String config_injector()    { return dir() + "/injector.txt";    }
    private String config_credentials() { return dir() + "/credentials.txt"; }

    private void writeIPNS(String s)       { Util.saveToFile(this, config_ipns(), s); }
    private void writeInjectorEP(String s) { Util.saveToFile(this, config_injector(), s); }

    public String readIPNS()       { return Util.readFromFile(this, config_ipns(), ""); }
    public String readInjectorEP() { return Util.readFromFile(this, config_injector(), ""); }

    private void writeCredentials(String injector, String cred) {
        Util.saveToFile(this, config_credentials(), injector + "\n" + cred);
    }

    private String readCredentialsFor(String injector) {
        if (injector == null || injector.length() == 0) return "";

        String content = Util.readFromFile(this, config_credentials(), null);

        if (content == null) { return ""; }

        String[] lines = content.split("\\n");

        if (lines.length != 2)          { return ""; }
        if (!lines[0].equals(injector)) { return ""; }

        return lines[1];
    }

    //----------------------------------------------------------------
}

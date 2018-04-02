package ie.equalit.ouinet;

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

import ie.equalit.ouinet.OuiWebViewClient;
import ie.equalit.ouinet.Util;
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

    protected String readIPNS() { return Util.readFromFile(this, "ipns.txt", ""); }
    protected void writeIPNS(String s) { Util.saveToFile(this, "ipns.txt", s); }

    protected String readInjectorEP() { return Util.readFromFile(this, "injector.txt", ""); }
    protected void writeInjectorEP(String s) { Util.saveToFile(this, "injector.txt", s); }

    protected void loadConfigFromQR() {
        IntentIntegrator integrator = new IntentIntegrator(this);
        integrator.initiateScan();
    }

    protected void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }

    protected void applyConfig(String config) {
        String[] lines = config.split("[\\r?\\n]+");

        for (String line:lines) {
            String[] keyval = line.trim().split("\\s*=\\s*", 2);

            if (keyval.length != 2) {
                continue;
            }

            String key = keyval[0];
            String val = keyval[1];

            if (key.equalsIgnoreCase("ipns")) {
                toast("Setting IPNS to: " + val);
                writeIPNS(val);
                _ouinet.setOuinetIPNS(val);
            }
            else if (key.equalsIgnoreCase("injector")) {
                toast("Setting injector to: " + val);
                writeInjectorEP(val);
                _webViewClient.forgetCredentials();
                _ouinet.setOuinetInjectorEP(val);
            }
            else {
                toast("Unknown config key: " + key);
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

        _ouinet = new Ouinet();

        setContentView(R.layout.activity_main);

        String ipns = readIPNS();
        if (ipns.length() > 0) { _ouinet.setOuinetIPNS(ipns); }

        String injector_ep = readInjectorEP();
        if (injector_ep.length() > 0) { _ouinet.setOuinetInjectorEP(injector_ep); }

        _ouinet.startOuinetClient( getFilesDir().getAbsolutePath()
                                 , injector_ep
                                 , ipns);

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

        _webViewClient = new OuiWebViewClient(this);
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
        showDialog("Injector endpoint", readInjectorEP(), new OnInput() {
            @Override
            public void call(String input) {
                writeInjectorEP(input);
                _webViewClient.forgetCredentials();
                _ouinet.setOuinetInjectorEP(input);
            }
        });
    }

    protected void showChangeIPNSDialog() {
        showDialog("IPNS", readIPNS(), new OnInput() {
            @Override
            public void call(String input) {
                writeIPNS(input);
                _ouinet.setOuinetIPNS(input);
            }
        });
    }

    @Override
    protected void onDestroy() {
        _ouinet.stopOuinetClient();
        super.onDestroy();
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(Menu.NONE, 1, Menu.NONE, "Home");
        menu.add(Menu.NONE, 2, Menu.NONE, "Reload");
        menu.add(Menu.NONE, 3, Menu.NONE, "Clear cache");
        menu.add(Menu.NONE, 4, Menu.NONE, "Injector endpoint");
        menu.add(Menu.NONE, 5, Menu.NONE, "IPNS");
        menu.add(Menu.NONE, 6, Menu.NONE, "Load config from QR");
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
                _webView.clearCache(true);
                return true;
            case 4:
                showChangeInjectorDialog();
                return true;
            case 5:
                showChangeIPNSDialog();
                return true;
            case 6:
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
}

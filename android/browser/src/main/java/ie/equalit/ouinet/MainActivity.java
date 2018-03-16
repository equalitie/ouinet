package ie.equalit.ouinet;

import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;

import android.view.Menu;
import android.view.MenuItem;

import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import android.util.Log;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.view.View.OnClickListener;
import android.widget.EditText;
import android.text.InputType;

import java.io.FileInputStream;
import java.io.FileOutputStream;

interface OnInput {
    public void call(String input);
}

public class MainActivity extends AppCompatActivity {

    private WebView _webView;

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("ipfs_bindings");
        System.loadLibrary("ipfs-cache");
        System.loadLibrary("client");
        System.loadLibrary("native-lib");

        System.setProperty("http.proxyHost", "127.0.0.1");
        System.setProperty("http.proxyPort", "8080");

        System.setProperty("https.proxyHost", "127.0.0.1");
        System.setProperty("https.proxyPort", "8080");
    }

    void refresh() {
        Log.d("Ouinet", "Refreshing");
        _webView.loadUrl("http://www.bbc.com");
    }

    protected void saveToFile(String filename, String value) {
        FileOutputStream outputStream;
        try {
            outputStream = openFileOutput(filename, MODE_PRIVATE);
            outputStream.write(value.getBytes());
            outputStream.close();
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    protected String readFromFile(String filename) {
        FileInputStream inputStream;
        try {
            inputStream = openFileInput(filename);
            StringBuffer content = new StringBuffer("");

            byte[] buffer = new byte[1024];

            int n;
            while ((n = inputStream.read(buffer)) != -1) {
                content.append(new String(buffer, 0, n));
            }

            return content.toString();
        } catch (Exception e) {
            return "";
        }
    }

    protected String readIPNS() { return readFromFile("ipns.txt"); }
    protected void writeIPNS(String s) { saveToFile("ipns.txt", s); }

    protected String readInjectorEP() { return readFromFile("injector.txt"); }
    protected void writeInjectorEP(String s) { saveToFile("injector.txt", s); }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        startOuinetClient(getFilesDir().getAbsolutePath());

        String ipns = readIPNS();
        if (ipns.length() > 0) { setOuinetIPNS(ipns); }

        String injector_ep = readInjectorEP();
        if (injector_ep.length() > 0) { setOuinetInjectorEP(injector_ep); }

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

        _webView.setWebViewClient(new WebViewClient());

        refresh();
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
                setOuinetInjectorEP(input);
            }
        });
    }

    protected void showChangeIPNSDialog() {
        showDialog("IPNS", readIPNS(), new OnInput() {
            @Override
            public void call(String input) {
                writeIPNS(input);
                setOuinetIPNS(input);
            }
        });
    }

    @Override
    protected void onDestroy() {
        stopOuinetClient();
        super.onDestroy();
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(Menu.NONE, 1, Menu.NONE, "Refresh");
        menu.add(Menu.NONE, 2, Menu.NONE, "Set injector endpoint");
        menu.add(Menu.NONE, 3, Menu.NONE, "Set INPS");
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        switch (item.getItemId()) {
            case 1:
                refresh();
                return true;
            case 2:
                showChangeInjectorDialog();
                return true;
            case 3:
                showChangeIPNSDialog();
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

    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    public native void startOuinetClient(String repo_root);
    public native void stopOuinetClient();
    public native void setOuinetInjectorEP(String endpoint);
    public native void setOuinetIPNS(String endpoint);
}

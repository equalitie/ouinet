package ie.equalit.ouinet;

import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.widget.TextView;

import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import android.util.Log;
import android.view.MotionEvent;

public class MainActivity extends AppCompatActivity {

    private WebView _webView;

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("ipfs_bindings");
        System.loadLibrary("ipfs-cache");
        System.loadLibrary("client");
        System.loadLibrary("native-lib");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        startOuinetClient(getFilesDir().getAbsolutePath());

        _webView = (WebView) findViewById(R.id.webview);
        _webView.setWebViewClient(new WebViewClient());
        WebSettings webSettings = _webView.getSettings();
        webSettings.setJavaScriptEnabled(true);

        _webView.loadUrl("http://www.bbc.com");
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        _webView.loadUrl("http://www.bbc.com");
        return super.onTouchEvent(event);
    }

    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    public native String startOuinetClient(String repo_root);
}

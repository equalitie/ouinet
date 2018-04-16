package ie.equalit.ouinet;

import android.content.Context;

import android.webkit.WebViewClient;
import android.webkit.WebView;
import android.webkit.HttpAuthHandler;
import android.webkit.WebViewDatabase;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;

import android.widget.EditText;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.view.View.OnClickListener;
import android.text.InputType;

import java.util.Map;
import java.util.HashMap;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;

class OuiWebViewClient extends WebViewClient {
    private Ouinet _ouinet;
    private Context _context;
    private boolean _showClientFrontEnd = false;

    public OuiWebViewClient(Context context, Ouinet ouinet) {
        super();
        _ouinet  = ouinet;
        _context = context;
    }

    public void toggleShowClientFrontend() {
        _showClientFrontEnd = !_showClientFrontEnd;
    }

    @Override
    public WebResourceResponse shouldInterceptRequest(WebView view, String url)
    {
        if (!_showClientFrontEnd)
            return super.shouldInterceptRequest(view, url);

        try {
            OkHttpClient httpClient = new OkHttpClient();

            Request request = new Request.Builder()
                    .url(url.trim())
                    .addHeader("X-Oui-Destination", "OuiClient")
                    .build();

            Response response = httpClient.newCall(request).execute();

            return new WebResourceResponse(
                    null,
                    response.header("content-encoding", "utf-8"),
                    response.body().byteStream());
        }
        catch (Exception e) {
            return null;
        }
    }

    @Override
    public void onReceivedHttpAuthRequest(WebView view,
                                          final HttpAuthHandler handler,
                                          String host,
                                          String realm)
    {
        final String cur_injector = _ouinet.getInjectorEndpoint();

        if (cur_injector == null || cur_injector.length() == 0) {
            handler.cancel();
            return;
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(_context);

        builder.setTitle("Credentials required for Ouinet injector");
        builder.setMessage("Insert credentials for " + cur_injector +
                           " in the <USENAME>:<PASSWORD> format");

        final EditText credentials = new EditText(_context);

        credentials.setInputType(InputType.TYPE_CLASS_TEXT);

        builder.setView(credentials);

        builder.setPositiveButton("OK", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                String userpass = credentials.getText().toString();
                if (userpass == null || userpass.length() == 0) { return; }
                _ouinet.setCredentialsFor(cur_injector, userpass);
                handler.proceed("", "");
            }
        });

        builder.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                handler.cancel();
                dialog.cancel();
            }
        });

        builder.show();
    }
}


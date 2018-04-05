package ie.equalit.ouinet;

import android.content.Context;

import android.webkit.WebViewClient;
import android.webkit.WebView;
import android.webkit.HttpAuthHandler;
import android.webkit.WebViewDatabase;

import android.widget.EditText;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.view.View.OnClickListener;
import android.text.InputType;

class OuiWebViewClient extends WebViewClient {
    private Ouinet _ouinet;
    private Context _context;

    public OuiWebViewClient(Context context, Ouinet ouinet) {
        super();
        _ouinet  = ouinet;
        _context = context;
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


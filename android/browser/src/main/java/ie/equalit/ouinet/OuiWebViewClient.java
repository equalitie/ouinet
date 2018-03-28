package ie.equalit.ouinet;

import android.content.Context;

import android.webkit.WebViewClient;
import android.webkit.WebView;
import android.webkit.HttpAuthHandler;

import android.widget.EditText;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.view.View.OnClickListener;
import android.text.InputType;

class OuiWebViewClient extends WebViewClient {
    private Context _context;

    public OuiWebViewClient(Context context) {
        super();
        _context = context;
    }

    @Override
    public void onReceivedHttpAuthRequest(WebView view,
                                          final HttpAuthHandler handler,
                                          String host,
                                          String realm)
    {
        AlertDialog.Builder builder = new AlertDialog.Builder(_context);

        builder.setTitle("Credentials required for Ouinet injector");
        builder.setMessage("Insert credentials in the <USENAME>:<PASSWORD> format");

        final EditText credentials = new EditText(_context);

        credentials.setInputType(InputType.TYPE_CLASS_TEXT);

        builder.setView(credentials);

        builder.setPositiveButton("OK", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                String credentials_str = credentials.getText().toString();
                String[] keyval = credentials_str.split(":", 2);

                if (keyval.length != 2) { return; }

                String username = keyval[0];
                String password = keyval[1];

                handler.proceed(username, password);
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
}


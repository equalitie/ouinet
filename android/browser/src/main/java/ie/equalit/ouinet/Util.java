package ie.equalit.ouinet;

import android.content.Context;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import android.util.Log;

class Util {
    public static void saveToFile(Context ctx, String filename, String value) {
        FileOutputStream outputStream;
        try {
            outputStream = ctx.openFileOutput(filename, ctx.MODE_PRIVATE);
            outputStream.write(value.getBytes());
            outputStream.close();
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    public static String readFromFile(Context ctx, String filename) {
        FileInputStream inputStream;
        try {
            inputStream = ctx.openFileInput(filename);
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

    public static void log(String s) {
        Log.d("Ouinet", s);
    }
}

package ie.equalit.ouinet;

import android.content.Context;
import android.os.Parcel;
import android.os.Parcelable;
import android.support.annotation.NonNull;
import android.support.annotation.Nullable;
import android.support.annotation.VisibleForTesting;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * Contains the configuration settings for the Ouinet service. The config is immutable and can
 * be passed around as a Parcelable. The Config has a private constructor and should only be
 * created by the ConfigBuilder, as building the config generates certificates and copies files
 * from the APK onto the filesystem. This should only be done once in the life time of the
 * app to avoid race conditions.
 */
public class Config implements Parcelable {
    public static enum LogLevel {
        SILLY,
        DEBUG,
        VERBOSE,
        INFO,
        WARN,
        ERROR,
        ABORT
    }

    private static final String TAG = "OuinetConfig";

    private static final String ASSET_PATH = "file:///android_asset/";
    @VisibleForTesting static final String OBFS4_PROXY = "obfs4proxy";
    private static final String OUINET_DIR = "/ouinet";

    public static class ConfigBuilder {
        private Context context;
        private String cacheHttpPubKey;
        private String injectorCredentials;
        private String injectorTlsCert;
        private String tlsCaCertStorePath;
        private String cacheType;
        private boolean cachePrivate = false;
        private String cacheStaticPath;
        private String cacheStaticContentPath;
        private boolean disableOriginAccess   = false;
        private boolean disableProxyAccess    = false;
        private boolean disableInjectorAccess = false;
        private LogLevel logLevel = LogLevel.INFO;

        public ConfigBuilder(Context context) {
            Ouinet.maybeLoadLibraries(context);

            this.context = context;
        }

        public ConfigBuilder setCacheHttpPubKey(String cacheHttpPubKey){
            this.cacheHttpPubKey = cacheHttpPubKey;
            return this;
        }
        public ConfigBuilder setInjectorCredentials(String injectorCredentials){
            this.injectorCredentials = injectorCredentials;
            return this;
        }
        public ConfigBuilder setInjectorTlsCert(String injectorTlsCert){
            this.injectorTlsCert = injectorTlsCert;
            return this;
        }
        /**
         * Path to a .pem file with CA certificates.
         * One can get it from e.g.: https://curl.haxx.se/docs/caextract.html
         */
        public ConfigBuilder setTlsCaCertStorePath(@Nullable String tlsCaCertStorePath){
            this.tlsCaCertStorePath = tlsCaCertStorePath;
            return this;
        }
        public ConfigBuilder setCacheType(String cacheType){
            this.cacheType = cacheType;
            return this;
        }
        public ConfigBuilder setCachePrivate(boolean cachePrivate){
            this.cachePrivate = cachePrivate;
            return this;
        }
        public ConfigBuilder setCacheStaticPath(String cacheStaticPath){
            this.cacheStaticPath = cacheStaticPath;
            return this;
        }
        public ConfigBuilder setCacheStaticContentPath(String cacheStaticContentPath){
            this.cacheStaticContentPath = cacheStaticContentPath;
            return this;
        }
        public ConfigBuilder setDisableOriginAccess(boolean disableOriginAccess){
            this.disableOriginAccess = disableOriginAccess;
            return this;
        }
        public ConfigBuilder setDisableProxyAccess(boolean disableProxyAccess){
            this.disableProxyAccess = disableProxyAccess;
            return this;
        }
        public ConfigBuilder setDisableInjectorAccess(boolean disableInjectorAccess){
            this.disableInjectorAccess = disableInjectorAccess;
            return this;
        }
        public ConfigBuilder setLogLevel(LogLevel logLevel){
            this.logLevel = logLevel;
            return this;
        }

        /**
         * Writes the injector TLS Cert Store to the filesystem if necessary and returns the path to the
         * certificate.
         */
        private @Nullable String setupInjectorTlsCert(String ouinetDirectory) {
            if (injectorTlsCert == null) {
                return null;
            }
            try {
                String tlsCertPath = ouinetDirectory + "/injector-tls-cert.pem";
                writeToFile(tlsCertPath, injectorTlsCert.getBytes());
                return tlsCertPath;
            } catch (IOException e) {
                Log.d(TAG, "Exception thrown while creating injector's cert file: ", e);
            }
            return null;
        }

        /**
         * Copies the TLS CA Cert Store to the filesystem if necessary and returns the path to the
         * certificate.
         */
        private @Nullable String setupTlsCaCertStore(String ouinetDirectory) {
            if (tlsCaCertStorePath == null || !tlsCaCertStorePath.startsWith(ASSET_PATH)) {
                // Nothing to be done.
                return tlsCaCertStorePath;
            }

            // TODO: Ouinet's C++ code doesn't yet have a way to read asset
            // files from the apk. As a temporary workaround we copy the
            // asset file to a regular file and the pass path to that to
            // the C++ code.
            String filename = tlsCaCertStorePath.substring(ASSET_PATH.length());
            String dest = ouinetDirectory + "/assets/" + filename;
            if (copyAssetToFile(tlsCaCertStorePath, dest)) {
                return dest;
            }
            return null;
        }

        /**
         * Generates the CA root certificate if it does not exist.
         * @param ouinetDirectory
         * @return The path to the certificate.
         */
        private @NonNull String setupCaRootCert(String ouinetDirectory) {
            return Ouinet.getCARootCert(ouinetDirectory);
        }

        /**
         * Copies the objfs proxy executable from the APK assets onto the local filesystem and
         * makes it executable.
         * @param ouinetDirectory
         * @return Returns the directory where the executable was placed, or null if it could not
         * write to the file.
         */
        private @Nullable String setupObfs4ProxyExecutable(String ouinetDirectory) {
            String src = ASSET_PATH + OBFS4_PROXY;
            String dest = ouinetDirectory + "/" + OBFS4_PROXY;
            if (copyExecutableToFile(src, dest)) {
                Log.d(TAG, "obfs4proxy copied to " + dest);
                return ouinetDirectory;
            }
            Log.d(TAG, "obfs4proxy not copied");
            return null;
        }

        private boolean copyAssetToFile(String asset, String dest){
            if (!asset.startsWith(ASSET_PATH)) {
                throw new IllegalArgumentException("Invalid asset path: " + asset);
            }
            String assetPath = asset.substring(ASSET_PATH.length());
            try {
                java.io.InputStream stream = context.getAssets().open(assetPath);
                int size = stream.available();
                byte[] buffer = new byte[size];
                stream.read(buffer);
                stream.close();
                writeToFile(dest, buffer);
            } catch (IOException e) {
                Log.d(TAG, "Failed to write asset \"" + asset + "\" to file \"" + dest + "\"", e);
                return false;
            }

            return true;
        }

        private void writeToFile(String path, byte[] bytes) throws IOException {
            File file = new File(path);

            if (!file.exists()) {
                File dir = file.getParentFile();
                if (!dir.exists()) {
                    dir.mkdirs();
                }
                file.createNewFile();
            }

            FileOutputStream stream = new FileOutputStream(file);
            stream.write(bytes);
            stream.close();
        }

        private boolean copyExecutableToFile(String asset, String path) {
            if (!copyAssetToFile(asset, path)) {
                return false;
            }
            File executable = new File(path);
            if (!executable.setExecutable(true)) {
                Log.d(TAG, "Failed to set executable for file: " + path);
                return false;
            }
            return true;
        }

        private String getOuinetDirectory() {
            return context.getFilesDir().getAbsolutePath() + OUINET_DIR;
        }

        public Config build() {
            String ouinetDirectory = getOuinetDirectory();
            new File(ouinetDirectory).mkdirs();

            return new Config(
                    ouinetDirectory,
                    cacheHttpPubKey,
                    injectorCredentials,
                    setupInjectorTlsCert(ouinetDirectory),
                    setupTlsCaCertStore(ouinetDirectory),
                    setupCaRootCert(ouinetDirectory),
                    setupObfs4ProxyExecutable(ouinetDirectory),
                    cacheType,
                    cachePrivate,
                    cacheStaticPath,
                    cacheStaticContentPath,
                    disableOriginAccess,
                    disableProxyAccess,
                    disableInjectorAccess,
                    logLevel);
        }
    }

    private String ouinetDirectory;
    private String cacheHttpPubKey;
    private String injectorCredentials;
    private String injectorTlsCertPath;
    private String tlsCaCertStorePath;
    private String caRootCertPath;
    private String obfs4ProxyPath;
    private String cacheType;
    private boolean cachePrivate;
    private String cacheStaticPath;
    private String cacheStaticContentPath;
    private boolean disableOriginAccess;
    private boolean disableProxyAccess;
    private boolean disableInjectorAccess;
    private LogLevel logLevel;

    private Config(String ouinetDirectory,
                  String cacheHttpPubKey,
                  String injectorCredentials,
                  String injectorTlsCertPath,
                  String tlsCaCertStorePath,
                  String caRootCertPath,
                  String obfs4ProxyPath,
                  String cacheType,
                  boolean cachePrivate,
                  String cacheStaticPath,
                  String cacheStaticContentPath,
                  boolean disableOriginAccess,
                  boolean disableProxyAccess,
                  boolean disableInjectorAccess,
                  LogLevel logLevel) {
        this.ouinetDirectory = ouinetDirectory;
        this.cacheHttpPubKey = cacheHttpPubKey;
        this.injectorCredentials = injectorCredentials;
        this.injectorTlsCertPath = injectorTlsCertPath;
        this.tlsCaCertStorePath = tlsCaCertStorePath;
        this.caRootCertPath = caRootCertPath;
        this.obfs4ProxyPath = obfs4ProxyPath;
        this.cacheType = cacheType;
        this.cachePrivate = cachePrivate;
        this.cacheStaticPath = cacheStaticPath;
        this.cacheStaticContentPath = cacheStaticContentPath;
        this.disableOriginAccess = disableOriginAccess;
        this.disableProxyAccess = disableProxyAccess;
        this.disableInjectorAccess = disableInjectorAccess;
        this.logLevel = logLevel;
    }
    public String getOuinetDirectory() {
        return ouinetDirectory;
    }
    public String getCacheHttpPubKey() {
        return cacheHttpPubKey;
    }
    public String getInjectorCredentials() {
        return injectorCredentials;
    }
    public String getInjectorTlsCertPath() {
        return injectorTlsCertPath;
    }
    public String getTlsCaCertStorePath() {
        return tlsCaCertStorePath;
    }
    public String getCaRootCertPath() {
        return caRootCertPath;
    }
    public String getObfs4ProxyPath() {
        return obfs4ProxyPath;
    }
    public String getCacheType() {
        return cacheType;
    }
    public boolean getCachePrivate() {
        return cachePrivate;
    }
    public String getCacheStaticPath() {
        return cacheStaticPath;
    }
    public String getCacheStaticContentPath() {
        return cacheStaticContentPath;
    }
    public boolean getDisableOriginAccess() {
        return disableOriginAccess;
    }
    public boolean getDisableProxyAccess() {
        return disableProxyAccess;
    }
    public boolean getDisableInjectorAccess() {
        return disableInjectorAccess;
    }
    public LogLevel getLogLevel() {
        return logLevel;
    }

    public static final Parcelable.Creator<Config> CREATOR
            = new Parcelable.Creator<Config>() {
        public Config createFromParcel(Parcel in) {
            return new Config(in);
        }

        public Config[] newArray(int size) {
            return new Config[size];
        }
    };

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel out, int flags) {
        out.writeString(ouinetDirectory);
        out.writeString(cacheHttpPubKey);
        out.writeString(injectorCredentials);
        out.writeString(injectorTlsCertPath);
        out.writeString(tlsCaCertStorePath);
        out.writeString(caRootCertPath);
        out.writeString(obfs4ProxyPath);
        out.writeString(cacheType);
        out.writeInt(cachePrivate ? 1 : 0);
        out.writeString(cacheStaticPath);
        out.writeString(cacheStaticContentPath);
        out.writeInt(disableOriginAccess ? 1 : 0);
        out.writeInt(disableProxyAccess ? 1 : 0);
        out.writeInt(disableInjectorAccess ? 1 : 0);
        // https://stackoverflow.com/a/48533385/273348
        out.writeInt(logLevel.ordinal());
    }
    private Config(Parcel in) {
        ouinetDirectory = in.readString();
        cacheHttpPubKey = in.readString();
        injectorCredentials = in.readString();
        injectorTlsCertPath = in.readString();
        tlsCaCertStorePath = in.readString();
        caRootCertPath = in.readString();
        obfs4ProxyPath = in.readString();
        cacheType = in.readString();
        cachePrivate = in.readInt() != 0;
        cacheStaticPath = in.readString();
        cacheStaticContentPath = in.readString();

        disableOriginAccess   = in.readInt() != 0;
        disableProxyAccess    = in.readInt() != 0;
        disableInjectorAccess = in.readInt() != 0;

        logLevel = LogLevel.values()[in.readInt()];
    }

}

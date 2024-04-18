package ie.equalit.ouinet;

import android.content.Context;
import android.content.res.AssetManager;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.runners.MockitoJUnitRunner;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Set;

import static org.assertj.core.api.Assertions.contentOf;
import static org.hamcrest.CoreMatchers.is;
import static org.hamcrest.MatcherAssert.assertThat;
import static org.mockito.Mockito.when;

@RunWith(MockitoJUnitRunner.class)
public class ConfigTest {
    private static final Set<String> BT_BOOTSTRAP_EXTRAS = new HashSet<>();
    private static final String CACHE_HTTP_PUB_KEY = "cachehttppubkey1234567890";
    private static final String INJECTOR_ENDPOINT = "injectorendpoint789123";
    private static final String INJECTOR_CREDENTIALS = "injectorcredentials29384293847928498492849284";
    private static final String INJECTOR_TLS_CERT = "injectortlscert123123123123123";
    private static final String TLS_CA_CERT_PATH = "file:///android_asset/tls-ca-cert.pem";
    private static final String TLS_CA_CERT = "tlscacertcontent";
    private static final String OBFS_PROXY_CONTENT = "obfs4assetcontent";
    private static final String CACHE_TYPE = "bep5-http";
    private static final String CACHE_STATIC_PATH = "static-cache";
    private static final String CACHE_STATIC_CONTENT_PATH = "static-cache/.ouinet";
    private static final String LISTEN_ON_TCP = "0.0.0.0:8077";
    private static final String FRONT_END_EP = "0.0.0.0:8078";
    private static final boolean DISABLE_BRIDGE_ANNOUNCEMENT = true;
    private static final String MAX_CACHED_AGE = "120";
    private static final String LOCAL_DOMAIN = "local.domain";
    private static final String ORIGIN_DOH_BASE = "0.0.0.0:8079";

    static {
        BT_BOOTSTRAP_EXTRAS.add("192.0.2.1");
        BT_BOOTSTRAP_EXTRAS.add("192.0.2.2:6882");
        BT_BOOTSTRAP_EXTRAS.add("[2001:db8::1]");
        BT_BOOTSTRAP_EXTRAS.add("[2001:db8::2]:6882");
        BT_BOOTSTRAP_EXTRAS.add("example.com");
        BT_BOOTSTRAP_EXTRAS.add("example.net:6882");
    }

    @Mock
    private Context mockContext;
    @Mock
    private AssetManager mockAssetManager;
    @Rule
    public TemporaryFolder tmpDir = new TemporaryFolder();

    @Test
    public void test_build() throws IOException {
        File filesDir = tmpDir.newFolder("files/");
        String ouinetDir = filesDir.getPath() + "/ouinet";
        String caRootCertPath = ouinetDir + "/ssl-ca-cert.pem";
        String injectorTlsCertPath = ouinetDir + "/injector-tls-cert.pem";
        String tlsCaCertPath = ouinetDir + "/assets/tls-ca-cert.pem";
        String obfsFilePath = ouinetDir + "/" + Config.OBFS4_PROXY;
        String cacheStaticPath = filesDir.getPath() + "/" + CACHE_STATIC_PATH;
        String cacheStaticContentPath = filesDir.getPath() + "/" + CACHE_STATIC_CONTENT_PATH;

        when(mockContext.getFilesDir()).thenReturn(filesDir);
        when(mockContext.getAssets()).thenReturn(mockAssetManager);
        InputStream obfsproxyIs =
                new ByteArrayInputStream(OBFS_PROXY_CONTENT.getBytes(StandardCharsets.UTF_8));
        when(mockAssetManager.open(Config.OBFS4_PROXY)).thenReturn(obfsproxyIs);
        InputStream tlsCaCertIs =
                new ByteArrayInputStream(TLS_CA_CERT.getBytes(StandardCharsets.UTF_8));
        when(mockAssetManager.open("tls-ca-cert.pem")).thenReturn(tlsCaCertIs);

        PowerMockito.mockStatic(Ouinet.class);
        when(Ouinet.getCARootCert(ouinetDir)).thenReturn(caRootCertPath);

        Config config = new Config.ConfigBuilder(mockContext)
                .setBtBootstrapExtras(BT_BOOTSTRAP_EXTRAS)
                .setCacheHttpPubKey(CACHE_HTTP_PUB_KEY)
                .setInjectorCredentials(INJECTOR_CREDENTIALS)
                .setInjectorTlsCert(INJECTOR_TLS_CERT)
                .setTlsCaCertStorePath(TLS_CA_CERT_PATH)
                .setCacheType(CACHE_TYPE)
                .setCacheStaticPath(cacheStaticPath)
                .setCacheStaticContentPath(cacheStaticContentPath)
                .setListenOnTcp(LISTEN_ON_TCP)
                .setFrontEndEp(FRONT_END_EP)
                .setDisableBridgeAnnouncement(DISABLE_BRIDGE_ANNOUNCEMENT)
                .setMaxCachedAge(MAX_CACHED_AGE)
                .setLocalDomain(LOCAL_DOMAIN)
                .setOriginDohBase(ORIGIN_DOH_BASE)
                .build();

        assertThat(config.getOuinetDirectory(), is(ouinetDir));
        assertThat(config.getBtBootstrapExtras(), is(BT_BOOTSTRAP_EXTRAS));
        assertThat(config.getCacheHttpPubKey(), is(CACHE_HTTP_PUB_KEY));
        assertThat(config.getInjectorCredentials(), is(INJECTOR_CREDENTIALS));
        assertThat(config.getCaRootCertPath(), is(caRootCertPath));
        assertThat(config.getCacheType(), is(CACHE_TYPE));
        assertThat(config.getCacheStaticPath(), is(cacheStaticPath));
        assertThat(config.getCacheStaticContentPath(), is(cacheStaticContentPath));

        assertThat(config.getListenOnTcp(), is(LISTEN_ON_TCP));
        assertThat(config.getFrontEndEp(), is(FRONT_END_EP));
        assertThat(config.getMaxCachedAge(), is(MAX_CACHED_AGE));
        assertThat(config.getLocalDomain(), is(LOCAL_DOMAIN));
        assertThat(config.getOriginDohBase(), is(ORIGIN_DOH_BASE));

        assertThat(config.getTlsCaCertStorePath(), is(tlsCaCertPath));
        assertThat(contentOf(new File(config.getTlsCaCertStorePath())), is(TLS_CA_CERT));

        assertThat(config.getInjectorTlsCertPath(), is(injectorTlsCertPath));
        assertThat(contentOf(new File(config.getInjectorTlsCertPath())), is(INJECTOR_TLS_CERT));

        assertThat(config.getObfs4ProxyPath(), is(ouinetDir));
        assertThat(contentOf(new File(obfsFilePath)), is(OBFS_PROXY_CONTENT));

        /*
        // TODO: building the notification fails in testing because it needs real context
        //       to get default strings, need to determine best way to test new features.
        NotificationConfig notificationConfig = new NotificationConfig.Builder(mockContext)
            .setHomeActivity("$packageName.$localClassName")
            .setNotificationIcons(R.drawable.ouinet_globe_pm)
            .build();

        OuinetBackground ouinetBackground = new OuinetBackground.Builder(mockContext)
            .setOuinetConfig(config)
            .setNotificationConfig(notificationConfig)
            .build();
        */
    }
}

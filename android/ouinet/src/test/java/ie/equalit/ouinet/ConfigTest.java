package ie.equalit.ouinet;

import android.content.Context;
import android.content.res.AssetManager;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.powermock.api.mockito.PowerMockito;
import org.powermock.core.classloader.annotations.PrepareForTest;
import org.powermock.core.classloader.annotations.SuppressStaticInitializationFor;
import org.powermock.modules.junit4.PowerMockRunner;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

import static org.assertj.core.api.Assertions.contentOf;
import static org.hamcrest.CoreMatchers.is;
import static org.hamcrest.MatcherAssert.assertThat;
import static org.mockito.Mockito.when;

@RunWith(PowerMockRunner.class)
@PrepareForTest({Ouinet.class})
@SuppressStaticInitializationFor("ie.equalit.ouinet.Ouinet")
public class ConfigTest {
    private static String CACHE_HTTP_PUB_KEY = "cachehttppubkey1234567890";
    private static String INJECTOR_ENDPOINT = "injectorendpoint789123";
    private static String INJECTOR_CREDENTIALS = "injectorcredentials29384293847928498492849284";
    private static String INJECTOR_TLS_CERT = "injectortlscert123123123123123";
    private static String TLS_CA_CERT_PATH = "file:///android_asset/tls-ca-cert.pem";
    private static String TLS_CA_CERT = "tlscacertcontent";
    private static String OBFS_PROXY_CONTENT = "obfs4assetcontent";
    private static String CACHE_TYPE = "bep5-http";
    private static String CACHE_STATIC_PATH = "static-cache";
    private static String CACHE_STATIC_CONTENT_PATH = "static-cache/.ouinet";

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
                .setCacheHttpPubKey(CACHE_HTTP_PUB_KEY)
                .setInjectorCredentials(INJECTOR_CREDENTIALS)
                .setInjectorTlsCert(INJECTOR_TLS_CERT)
                .setTlsCaCertStorePath(TLS_CA_CERT_PATH)
                .setCacheType(CACHE_TYPE)
                .setCacheStaticPath(cacheStaticPath)
                .setCacheStaticContentPath(cacheStaticContentPath)
                .build();

        assertThat(config.getOuinetDirectory(), is(ouinetDir));
        assertThat(config.getCacheHttpPubKey(), is(CACHE_HTTP_PUB_KEY));
        assertThat(config.getInjectorCredentials(), is(INJECTOR_CREDENTIALS));
        assertThat(config.getCaRootCertPath(), is(caRootCertPath));
        assertThat(config.getCacheType(), is(CACHE_TYPE));
        assertThat(config.getCacheStaticPath(), is(cacheStaticPath));
        assertThat(config.getCacheStaticContentPath(), is(cacheStaticContentPath));

        assertThat(config.getTlsCaCertStorePath(), is(tlsCaCertPath));
        assertThat(contentOf(new File(config.getTlsCaCertStorePath())), is(TLS_CA_CERT));

        assertThat(config.getInjectorTlsCertPath(), is(injectorTlsCertPath));
        assertThat(contentOf(new File(config.getInjectorTlsCertPath())), is(INJECTOR_TLS_CERT));

        assertThat(config.getObfs4ProxyPath(), is(ouinetDir));
        assertThat(contentOf(new File(obfsFilePath)), is(OBFS_PROXY_CONTENT));
    }
}

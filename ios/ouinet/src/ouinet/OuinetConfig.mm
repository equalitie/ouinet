#import "Ouinet.h"

@interface OuinetConfig()
- (NSString*)setupInjectorTlsCert:(NSString*)ouinetDirectory;
@end

@implementation OuinetConfig
{
  NSString* ouinetDirectory;
  NSString* cacheHttpPubKey;
  NSString* injectorCredentials;
  NSString* injectorTlsCert;
  NSString* injectorTlsCertPath;
  NSString* tlsCaCertStoreDir;
  NSString* tlsCaCertCn;
  NSString* cacheType;
  NSString* listenOnTcp;
  NSString* frontEndEp;
  NSString* frontEndAccessToken;
  BOOL metricsEnableOnStart;
  NSString* metricsServerUrl;
  NSString* metricsServerToken;
  NSString* metricsServerTlsCaCert;
  NSString* metricsEncryptionKey;
  NSString* metricsDeleteAfter;
  NSString* logLevel;
  BOOL disableOriginAccess;
  BOOL disableProxyAccess;
  BOOL disableInjectorAccess;
  BOOL disableBridgeAnnouncement;
  BOOL disableDoH;
  BOOL disableUpnp;
  BOOL disableLocalPeerDiscovery;
  BOOL enableLogFile;
  NSArray<NSString*>* btBootstrapExtras;
  NSString* clientCredentials;
  NSString* proxyAccessToken;
  BOOL cachePrivate;
  NSString* cacheStaticPath;
  NSString* cacheStaticContentPath;
  NSString* maxCachedAge;
  NSString* requestBodyLimit;
  NSString* localDomain;
  NSArray<NSString*>* dnsProtocols;
  NSString* udpMuxPort;
  NSString* udpMuxRxLimit;
}

- (OuinetConfig*)init
{
  if (!(self = [super init]))
    return nil;
  NSError *error = nil;
  NSFileManager *fileManager = [NSFileManager defaultManager];
  NSString *documentDir = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) lastObject];
  ouinetDirectory = [NSString stringWithFormat: @"%@%@", documentDir, @"/ouinet"];
  NSString* ouinetConfFile = [NSString stringWithFormat: @"%@%@", ouinetDirectory, @"/ouinet-client.conf"];
  if(![fileManager createDirectoryAtPath:ouinetDirectory withIntermediateDirectories:YES attributes:nil error:&error]) {
    // An error has occurred, do something to handle it
    NSLog(@"Failed to create directory \"%@\". Error: %@", ouinetDirectory, error);
  }
  NSLog( @"text: Got ouinetDir: %@", ouinetDirectory);
  NSString *content = @"";
  NSData *fileContents = [content dataUsingEncoding:NSUTF8StringEncoding];
  [fileManager createFileAtPath:ouinetConfFile
                                contents:fileContents
                                attributes:nil];
  return self;
}

- (OuinetConfig*)setCacheHttpPubKey:(NSString*)key
{
  cacheHttpPubKey=key;
  return self;
}

- (OuinetConfig*)setInjectorCredentials:(NSString*)credentials
{
  injectorCredentials=credentials;
  return self;
}

- (OuinetConfig*)setInjectorTlsCert:(NSString*)cert
{
  injectorTlsCert = cert;
  injectorTlsCertPath = [self setupInjectorTlsCert:ouinetDirectory]; 
  return self;
}

- (OuinetConfig*)setCacheType:(NSString*)type
{
  cacheType=type;
  return self;
}


- (OuinetConfig*)setListenOnTcp:(NSString*)address
{
  listenOnTcp = address;
  return self;
}

- (OuinetConfig*)setFrontEndEp:(NSString*)address
{
  frontEndEp = address;
  return self;
}

- (OuinetConfig*)setFrontEndAccessToken:(NSString*)token
{
  frontEndAccessToken = token;
  return self;
}

- (OuinetConfig*)setTlsCaCertCn:(NSString*)cn
{
  tlsCaCertCn = cn;
  return self;
}

- (OuinetConfig*)setMetricsEnableOnStart:(BOOL)value
{
  metricsEnableOnStart = value;
  return self;
}

- (OuinetConfig*)setMetricsServerUrl:(NSString*)url
{
  metricsServerUrl = url;
  return self;
}

- (OuinetConfig*)setMetricsServerToken:(NSString*)token
{
  metricsServerToken = token;
  return self;
}

- (OuinetConfig*)setMetricsServerTlsCaCert:(NSString*)caCert
{
  metricsServerTlsCaCert = caCert;
  return self;
}

- (OuinetConfig*)setMetricsEncryptionKey:(NSString*)key
{
  metricsEncryptionKey = key;
  return self;
}

- (OuinetConfig*)setMetricsDeleteAfter:(NSString*)duration
{
  metricsDeleteAfter = duration;
  return self;
}

- (OuinetConfig*)setLogLevel:(NSString*)level
{
  logLevel = level;
  return self;
}

- (OuinetConfig*)setEnableLogFile:(BOOL)value
{
  enableLogFile = value;
  return self;
}

- (OuinetConfig*)setBtBootstrapExtras:(NSArray<NSString*>*)extras
{
  btBootstrapExtras = extras;
  return self;
}

- (OuinetConfig*)setClientCredentials:(NSString*)credentials
{
  clientCredentials = credentials;
  return self;
}

- (OuinetConfig*)setProxyAccessToken:(NSString*)token
{
  proxyAccessToken = token;
  return self;
}

- (OuinetConfig*)setCachePrivate:(BOOL)value
{
  cachePrivate = value;
  return self;
}

- (OuinetConfig*)setCacheStaticPath:(NSString*)path
{
  cacheStaticPath = path;
  return self;
}

- (OuinetConfig*)setCacheStaticContentPath:(NSString*)path
{
  cacheStaticContentPath = path;
  return self;
}

- (OuinetConfig*)setMaxCachedAge:(NSString*)maxCachedAge_
{
  maxCachedAge = maxCachedAge_;
  return self;
}

- (OuinetConfig*)setRequestBodyLimit:(NSString*)limit
{
  requestBodyLimit = limit;
  return self;
}

- (OuinetConfig*)setLocalDomain:(NSString*)domain
{
  localDomain = domain;
  return self;
}

- (OuinetConfig*)setDnsProtocols:(NSArray<NSString*>*)protocols
{
  dnsProtocols = protocols;
  return self;
}

- (OuinetConfig*)setUdpMuxPort:(NSString*)port
{
  udpMuxPort = port;
  return self;
}

- (OuinetConfig*)setUdpMuxRxLimit:(NSString*)limit
{
  udpMuxRxLimit = limit;
  return self;
}

- (OuinetConfig*)setDisableOriginAccess:(BOOL)value
{
  disableOriginAccess = value;
  return self;
}

- (OuinetConfig*)setDisableProxyAccess:(BOOL)value;
{
  disableProxyAccess = value;
  return self;
}

- (OuinetConfig*)setDisableInjectorAccess:(BOOL)value;
{
  disableInjectorAccess = value;
  return self;
}

- (OuinetConfig*)setDisableBridgeAnnouncement:(BOOL)value;
{
  disableBridgeAnnouncement = value;
  return self;
}

- (OuinetConfig*)setDisableDoH:(BOOL)value;
{
  disableDoH = value;
  return self;
}

- (OuinetConfig*)setDisableUpnp:(BOOL)value;
{
  disableUpnp = value;
  return self;
}

- (OuinetConfig*)setDisableLocalPeerDiscovery:(BOOL)value;
{
  disableLocalPeerDiscovery = value;
  return self;
}

- (NSString*)getOuinetDirectory
{
  return ouinetDirectory;
}

- (NSString*)getCacheHttpPubKey
{
  return cacheHttpPubKey;
}

- (NSString*)getInjectorCredentials
{
  return injectorCredentials;
}

- (NSString*)getInjectorTlsCertPath
{
  return injectorTlsCertPath;
}

- (NSString*)getTlsCaCertStoreDir;
{
  return [NSString stringWithFormat: @"%@/cacert.pem", ouinetDirectory];
}

- (NSString*)getTlsCaCertCn
{
  return tlsCaCertCn;
}

- (NSString*)getCacheType
{
  return cacheType;
}

- (NSString*)getListenOnTcp
{
  return listenOnTcp;
}

- (NSString*)getFrontEndEp
{
  return frontEndEp;
}

- (NSString*)getFrontEndAccessToken
{
  return frontEndAccessToken;
}

- (BOOL)getMetricsEnableOnStart
{
  return metricsEnableOnStart;
}

- (NSString*)getMetricsServerUrl
{
  return metricsServerUrl;
}

- (NSString*)getMetricsServerToken
{
  return metricsServerToken;
}

- (NSString*)getMetricsServerTlsCaCert
{
  return metricsServerTlsCaCert;
}

- (NSString*)getMetricsEncryptionKey
{
  return metricsEncryptionKey;
}

- (NSString*)getMetricsDeleteAfter
{
  return metricsDeleteAfter;
}

- (NSString*)getLogLevel
{
  return logLevel;
}

- (BOOL)getDisableOriginAccess
{
  return disableOriginAccess;
}

- (BOOL)getDisableProxyAccess
{
  return disableProxyAccess;
}

- (BOOL)getDisableInjectorAccess
{
  return disableInjectorAccess;
}

- (BOOL)getDisableBridgeAnnouncement
{
  return disableBridgeAnnouncement;
}

- (BOOL)getDisableDoH
{
  return disableDoH;
}

- (BOOL)getDisableUpnp
{
  return disableUpnp;
}

- (BOOL)getDisableLocalPeerDiscovery
{
  return disableLocalPeerDiscovery;
}

- (BOOL)getEnableLogFile
{
  return enableLogFile;
}

- (NSString*)getLogFilePath
{
  if (!enableLogFile) {
    return nil;
  }
  return [NSString stringWithFormat: @"%@/log.txt", ouinetDirectory];
}

- (NSArray<NSString*>*)getBtBootstrapExtras
{
  return btBootstrapExtras;
}

- (NSString*)getClientCredentials
{
  return clientCredentials;
}

- (NSString*)getProxyAccessToken
{
  return proxyAccessToken;
}

- (BOOL)getCachePrivate
{
  return cachePrivate;
}

- (NSString*)getCacheStaticPath
{
  return cacheStaticPath;
}

- (NSString*)getCacheStaticContentPath
{
  return cacheStaticContentPath;
}

- (NSString*)getMaxCachedAge
{
  return maxCachedAge;
}

- (NSString*)getRequestBodyLimit
{
  return requestBodyLimit;
}

- (NSString*)getLocalDomain
{
  return localDomain;
}

- (NSArray<NSString*>*)getDnsProtocols
{
  return dnsProtocols;
}

- (NSString*)getUdpMuxPort
{
  return udpMuxPort;
}

- (NSString*)getUdpMuxRxLimit
{
  return udpMuxRxLimit;
}

/**
  * Writes the injector TLS Cert Store to the filesystem if necessary and returns the path to the
  * certificate.
  */
- (NSString*)setupInjectorTlsCert:(NSString*)ouinetDir
{
  if (injectorTlsCert == nil) {
      return nil;
  }
  NSString* tlsCertPath = [NSString stringWithFormat: @"%@%@", ouinetDir, @"/injector-tls-cert.pem"];
  NSString *content = injectorTlsCert;
  NSData *fileContents = [content dataUsingEncoding:NSUTF8StringEncoding];
  [[NSFileManager defaultManager] createFileAtPath:tlsCertPath
                                contents:fileContents
                                attributes:nil];
  return tlsCertPath;
}

@end

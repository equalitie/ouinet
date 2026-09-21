#import <Foundation/Foundation.h>

@interface OuinetConfig : NSObject

- (OuinetConfig*)init;

- (OuinetConfig*)setCacheHttpPubKey:(NSString*)key;

- (OuinetConfig*)setInjectorCredentials:(NSString*)credentials;

- (OuinetConfig*)setInjectorTlsCert:(NSString*)cert;

- (OuinetConfig*)setCacheType:(NSString*)type;

- (OuinetConfig*)setListenOnTcp:(NSString*)address;

- (OuinetConfig*)setFrontEndEp:(NSString*)address;

- (OuinetConfig*)setFrontEndAccessToken:(NSString*)token;

- (OuinetConfig*)setMetricsEnableOnStart:(BOOL)value;

- (OuinetConfig*)setMetricsServerUrl:(NSString*)url;

- (OuinetConfig*)setMetricsServerToken:(NSString*)token;

- (OuinetConfig*)setMetricsServerTlsCaCert:(NSString*)caCert;

- (OuinetConfig*)setMetricsEncryptionKey:(NSString*)key;

- (OuinetConfig*)setMetricsDeleteAfter:(NSString*)duration;

- (OuinetConfig*)setDisableOriginAccess:(BOOL)value;

- (OuinetConfig*)setDisableProxyAccess:(BOOL)value;

- (OuinetConfig*)setDisableInjectorAccess:(BOOL)value;

- (OuinetConfig*)setDisableBridgeAnnouncement:(BOOL)value;

- (OuinetConfig*)setDisableDoH:(BOOL)value;

- (OuinetConfig*)setDisableUpnp:(BOOL)value;

- (OuinetConfig*)setDisableLocalPeerDiscovery:(BOOL)value;

- (OuinetConfig*)setLogLevel:(NSString*)level;

- (OuinetConfig*)setEnableLogFile:(BOOL)value;

- (OuinetConfig*)setBtBootstrapExtras:(NSArray<NSString*>*)extras;

- (OuinetConfig*)setClientCredentials:(NSString*)credentials;

- (OuinetConfig*)setProxyAccessToken:(NSString*)token;

- (OuinetConfig*)setCachePrivate:(BOOL)value;

- (OuinetConfig*)setCacheStaticPath:(NSString*)path;

- (OuinetConfig*)setCacheStaticContentPath:(NSString*)path;

- (OuinetConfig*)setMaxCachedAge:(NSString*)maxCachedAge;

- (OuinetConfig*)setRequestBodyLimit:(NSString*)limit;

- (OuinetConfig*)setLocalDomain:(NSString*)domain;

- (OuinetConfig*)setDnsProtocols:(NSArray<NSString*>*)protocols;

- (OuinetConfig*)setUdpMuxPort:(NSString*)port;

- (OuinetConfig*)setUdpMuxRxLimit:(NSString*)limit;

- (NSString*)getOuinetDirectory;

- (NSString*)getCacheHttpPubKey;

- (NSString*)getInjectorCredentials;

- (NSString*)getInjectorTlsCertPath;

- (NSString*)getTlsCaCertStoreDir;

- (NSString*)getCacheType;

- (NSString*)getListenOnTcp;

- (NSString*)getFrontEndEp;

- (NSString*)getFrontEndAccessToken;

- (BOOL)getMetricsEnableOnStart;

- (NSString*)getMetricsServerUrl;

- (NSString*)getMetricsServerToken;

- (NSString*)getMetricsServerTlsCaCert;

- (NSString*)getMetricsEncryptionKey;

- (NSString*)getMetricsDeleteAfter;

- (NSString*)getLogLevel;

- (BOOL)getDisableOriginAccess;

- (BOOL)getDisableProxyAccess;

- (BOOL)getDisableInjectorAccess;

- (BOOL)getDisableBridgeAnnouncement;

- (BOOL)getDisableDoH;

- (BOOL)getDisableUpnp;

- (BOOL)getDisableLocalPeerDiscovery;

- (BOOL)getEnableLogFile;

- (NSString*)getLogFilePath;

- (NSArray<NSString*>*)getBtBootstrapExtras;

- (NSString*)getClientCredentials;

- (NSString*)getProxyAccessToken;

- (BOOL)getCachePrivate;

- (NSString*)getCacheStaticPath;

- (NSString*)getCacheStaticContentPath;

- (NSString*)getMaxCachedAge;

- (NSString*)getRequestBodyLimit;

- (NSString*)getLocalDomain;

- (NSArray<NSString*>*)getDnsProtocols;

- (NSString*)getUdpMuxPort;

- (NSString*)getUdpMuxRxLimit;

@end

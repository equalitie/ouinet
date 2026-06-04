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

- (OuinetConfig*)setLogLevel:(NSString*)level;

- (NSString*)getOuinetDirectory;

- (NSString*)getCacheHttpPubKey;

- (NSString*)getInjectorCredentials;

- (NSString*)getInjectorTlsCertPath;

- (NSString*)getTlsCaCertStorePath;

- (NSString*)getCacheType;

- (NSString*)getListenOnTcp;

- (NSString*)getFrontEndEp;

- (NSString*)getFrontEndAccessToken;

- (BOOL)getMetricsEnableOnStart;

- (NSString*)getMetricsServerUrl;

- (NSString*)getMetricsServerToken;

- (NSString*)getMetricsServerTlsCaCertPath;

- (NSString*)getMetricsEncryptionKey;

- (NSString*)getMetricsDeleteAfter;

- (NSString*)getLogLevel;

- (BOOL)getDisableOriginAccess;

- (BOOL)getDisableProxyAccess;

- (BOOL)getDisableInjectorAccess;

- (BOOL)getDisableBridgeAnnouncement;

- (BOOL)getDisableDoH;

@end

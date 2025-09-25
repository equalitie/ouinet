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
  NSString* tlsCaCertStorePath;
  NSString* cacheType;
  NSString* listenOnTcp;
  NSString* frontEndEp;
  NSString* logLevel;
  BOOL disableOriginAccess;
  BOOL disableProxyAccess;
  BOOL disableInjectorAccess;
  BOOL disableBridgeAnnouncement;
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

- (OuinetConfig*)setLogLevel:(NSString*)level
{
  logLevel = level;
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

- (NSString*)getTlsCaCertStorePath;
{
  return [NSString stringWithFormat: @"%@/cacert.pem", ouinetDirectory];
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
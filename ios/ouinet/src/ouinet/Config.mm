#import "Ouinet.h"

@interface Config()
- (NSString*)setupInjectorTlsCert:(NSString*)ouinetDirectory;
@end

@implementation Config
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
  BOOL disableOriginAccess;
}

- (Config*)init
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

- (Config*)setCacheHttpPubKey:(NSString*)key
{
  cacheHttpPubKey=key;
  return self;
}

- (Config*)setInjectorCredentials:(NSString*)credentials
{
  injectorCredentials=credentials;
  return self;
}

- (Config*)setInjectorTlsCert:(NSString*)cert
{
  injectorTlsCert = cert;
  injectorTlsCertPath = [self setupInjectorTlsCert:ouinetDirectory]; 
  return self;
}

- (Config*)setCacheType:(NSString*)type
{
  cacheType=type;
  return self;
}


- (Config*)setListenOnTcp:(NSString*)address
{
  listenOnTcp = address;
  return self;
}

- (Config*)setFrontEndEp:(NSString*)address
{
  frontEndEp = address;
  return self;
}

- (Config*)setDisableOriginAccess:(BOOL)value
{
  disableOriginAccess = value;
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

- (BOOL)getDisableOriginAccess
{
  return disableOriginAccess;
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
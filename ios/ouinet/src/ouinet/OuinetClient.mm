#include "native-lib.hpp"
#import "Ouinet.h"

NativeLib _n;

@interface OuinetClient()
- (std::vector<std::string>)maybeAdd:(std::vector<std::string>)args stringOfKey:(NSString*)key stringOfValue:(NSString*)value;
@end

@implementation OuinetClient
{
    OuinetConfig* config;
}

- (id)initWithConfig:(OuinetConfig*)conf
{
    if (!(self = [super init]))
        return nil;
    config = conf;
    return self;
}

- (NSNumber*)getClientState
{
  return [NSNumber numberWithInt: _n.getClientState()];
}

- (NSString*)getHelloOuinet
{
  return [NSString stringWithUTF8String: _n.helloOuinet().c_str()];
}

- (void)start
{
  NSLog( @"text: %@", @"Ouinet start request begin");
  NSError *error = nil;

  std::vector<std::string> args;
  args.push_back("ouinet-client");
  args.push_back(std::string([[NSString stringWithFormat: @"--repo=%@", [config getOuinetDirectory]] UTF8String]));
  args = [self maybeAdd:args stringOfKey:@"--injector-credentials" stringOfValue:[config getInjectorCredentials]];
  args = [self maybeAdd:args stringOfKey:@"--listen-on-tcp" stringOfValue:[config getListenOnTcp]];
  args = [self maybeAdd:args stringOfKey:@"--front-end-ep" stringOfValue:[config getFrontEndEp]];
  args = [self maybeAdd:args stringOfKey:@"--cache-http-public-key" stringOfValue:[config getCacheHttpPubKey]];
  args = [self maybeAdd:args stringOfKey:@"--cache-type" stringOfValue:[config getCacheType]];
  args = [self maybeAdd:args stringOfKey:@"--injector-tls-cert-file" stringOfValue:[config getInjectorTlsCertPath]];
  args = [self maybeAdd:args stringOfKey:@"--tls-ca-cert-store-path" stringOfValue:[config getTlsCaCertStorePath]];
  args.push_back(std::string([[NSString stringWithFormat: @"%@", @"--log-level=DEBUG"] UTF8String]));
  if ([config getDisableOriginAccess]) {
    args.push_back("--disable-origin-access");
  }
  if ([config getDisableProxyAccess]) {
    args.push_back("--disable-proxy-access");
  }
  if ([config getDisableInjectorAccess]) {
    args.push_back("--disable-injector-access");
  }
  NSString *certFileContents = [NSString stringWithContentsOfFile:[config getInjectorTlsCertPath] encoding:NSUTF8StringEncoding error:&error];
  if (error)
    NSLog(@"Error reading file: %@", error.localizedDescription);

  // maybe for debugging...
  NSLog(@"contents: %@", certFileContents);

  _n.startClient(args);
  NSLog( @"text: %@", @"Ouinet start request complete");
  return;
}


- (std::vector<std::string>)maybeAdd:(std::vector<std::string>)args stringOfKey:(NSString*)key stringOfValue:(NSString*)value
{
  if (value == nil || [value isEqualToString:@""]) {
    return args;
  }
  args.push_back(std::string([[NSString stringWithFormat: @"%@=%@", key, value] UTF8String]));
  return args;
}

@end

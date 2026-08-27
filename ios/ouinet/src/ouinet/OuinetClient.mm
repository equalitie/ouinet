#include "native-lib.hpp"
#import "Ouinet.h"

NativeLib _n;

@interface OuinetClient()
- (std::vector<std::string>)maybeAdd:(std::vector<std::string>)args stringOfKey:(NSString*)key stringOfValue:(NSString*)value;
- (std::vector<std::string>)maybeAdd:(std::vector<std::string>)args stringOfKey:(NSString*)key arrayOfValues:(NSArray<NSString*>*)values;
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

- (void)start
{
  NSLog( @"text: %@", @"Ouinet start request begin");
  NSError *error = nil;

  std::vector<std::string> args;
  args.push_back("ouinet-client");
  args.push_back(std::string([[NSString stringWithFormat: @"--repo=%@", [config getOuinetDirectory]] UTF8String]));
  args = [self maybeAdd:args stringOfKey:@"--bt-bootstrap-extra" arrayOfValues:[config getBtBootstrapExtras]];
  args = [self maybeAdd:args stringOfKey:@"--injector-credentials" stringOfValue:[config getInjectorCredentials]];
  args = [self maybeAdd:args stringOfKey:@"--client-credentials" stringOfValue:[config getClientCredentials]];
  args = [self maybeAdd:args stringOfKey:@"--listen-on-tcp" stringOfValue:[config getListenOnTcp]];
  args = [self maybeAdd:args stringOfKey:@"--udp-mux-port" stringOfValue:[config getUdpMuxPort]];
  args = [self maybeAdd:args stringOfKey:@"--udp-mux-rx-limit" stringOfValue:[config getUdpMuxRxLimit]];
  args = [self maybeAdd:args stringOfKey:@"--front-end-ep" stringOfValue:[config getFrontEndEp]];
  args = [self maybeAdd:args stringOfKey:@"--front-end-access-token" stringOfValue:[config getFrontEndAccessToken]];
  args = [self maybeAdd:args stringOfKey:@"--proxy-access-token" stringOfValue:[config getProxyAccessToken]];
  args = [self maybeAdd:args stringOfKey:@"--cache-http-public-key" stringOfValue:[config getCacheHttpPubKey]];
  args = [self maybeAdd:args stringOfKey:@"--cache-type" stringOfValue:[config getCacheType]];
  args = [self maybeAdd:args stringOfKey:@"--cache-static-repo" stringOfValue:[config getCacheStaticPath]];
  args = [self maybeAdd:args stringOfKey:@"--cache-static-root" stringOfValue:[config getCacheStaticContentPath]];
  args = [self maybeAdd:args stringOfKey:@"--max-cached-age" stringOfValue:[config getMaxCachedAge]];
  args = [self maybeAdd:args stringOfKey:@"--request-body-limit" stringOfValue:[config getRequestBodyLimit]];
  args = [self maybeAdd:args stringOfKey:@"--local-domain" stringOfValue:[config getLocalDomain]];
  args = [self maybeAdd:args stringOfKey:@"--dns-protocol" arrayOfValues:[config getDnsProtocols]];
  args = [self maybeAdd:args stringOfKey:@"--injector-tls-cert-file" stringOfValue:[config getInjectorTlsCertPath]];
  args = [self maybeAdd:args stringOfKey:@"--tls-ca-cert-store-path" stringOfValue:[config getTlsCaCertStorePath]];
  args = [self maybeAdd:args stringOfKey:@"--log-level" stringOfValue:[config getLogLevel]];
  if ([config getEnableLogFile]) {
    args.push_back("--enable-log-file");
  }
  if ([config getCachePrivate]) {
    args.push_back("--cache-private");
  }
  if ([config getDisableOriginAccess]) {
    args.push_back("--disable-origin-access");
  }
  if ([config getDisableProxyAccess]) {
    args.push_back("--disable-proxy-access");
  }
  if ([config getDisableInjectorAccess]) {
    args.push_back("--disable-injector-access");
  }
  if ([config getDisableCacheAccess]) {
    args.push_back("--disable-cache-access");
  }
  if ([config getDisableBridgeAnnouncement]) {
    args.push_back("--disable-bridge-announcement");
  }
  if ([config getDisableDoH]) {
    args.push_back("--disable-doh");
  }
  if ([config getDisableUpnp]) {
    args.push_back("--disable-upnp");
  }
  if ([config getDisableLocalPeerDiscovery]) {
    args.push_back("--disable-local-peer-discovery");
  }
  if ([config getMetricsEnableOnStart]) {
    args.push_back("--metrics-enable-on-start");
  }
  args = [self maybeAdd:args stringOfKey:@"--metrics-server-url" stringOfValue:[config getMetricsServerUrl]];
  args = [self maybeAdd:args stringOfKey:@"--metrics-server-token" stringOfValue:[config getMetricsServerToken]];
  args = [self maybeAdd:args stringOfKey:@"--metrics-encryption-key" stringOfValue:[config getMetricsEncryptionKey]];
  args = [self maybeAdd:args stringOfKey:@"--metrics-server-cacert-file" stringOfValue:[config getMetricsServerTlsCaCertPath]];
  args = [self maybeAdd:args stringOfKey:@"--metrics-delete-after" stringOfValue:[config getMetricsDeleteAfter]];
  
  NSString *certFileContents = [NSString stringWithContentsOfFile:[config getInjectorTlsCertPath] encoding:NSUTF8StringEncoding error:&error];
  if (error)
    NSLog(@"Error reading file: %@", error.localizedDescription);

  // maybe for debugging...
  NSLog(@"contents: %@", certFileContents);

  _n.startClient(args);
  NSLog( @"text: %@", @"Ouinet start request complete");
  return;
}

- (void)stop
{
  _n.stopClient();
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

- (std::vector<std::string>)maybeAdd:(std::vector<std::string>)args stringOfKey:(NSString*)key arrayOfValues:(NSArray<NSString*>*)values
{
  if (values == nil) {
    return args;
  }
  for (NSString* value in values) {
    args = [self maybeAdd:args stringOfKey:key stringOfValue:value];
  }
  return args;
}

- (NSString*)getProxyEndpoint
{
  std::string endpoint = _n.getProxyEndpoint();
  return [NSString stringWithUTF8String:endpoint.c_str()];
}

- (NSString*)getFrontendEndpoint
{
  std::string endpoint = _n.getFrontendEndpoint();
  return [NSString stringWithUTF8String:endpoint.c_str()];
}

@end

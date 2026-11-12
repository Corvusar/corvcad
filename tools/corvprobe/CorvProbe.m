// CorvProbe: a development-only LiveContainer tweak that measures CorvCAD on a physical iPad
// (gate 4). Loaded by LiveContainer's TweakLoader into the guest app, it stays inert in any
// other app. It samples the process memory footprint every 100 ms and serves JSON over HTTP on
// port 8765 to the local network. Every request needs ?token=<value of Documents/corvprobe.token>,
// which the tweak creates on first launch (readable in Files → CorvCAD).
//
//   GET  /metrics                     footprint, peak, available memory, uptime, bundle size, device
//   GET  /timeline?since=<ms>         memory samples [[ms since launch, footprint bytes], ...]
//   GET  /fonts                       what the app can read under /System/Library/Fonts
//   GET  /ls?path=<rel>               list a folder under Documents
//   GET  /get?path=<rel>              download a file under Documents
//   POST /put?path=<rel>              upload a file under Documents (creates folders)
//   GET  /run?command=schematic|board&path=<rel>
//                                     ask the app (Diagnostics.swift, CORVCAD_DIAGNOSTICS builds) to
//                                     load it; adds memory before, after and peak during the run
//   GET  /log?seconds=<n>&limit=<n>  the app's own os_log entries (OSLogStore) and its recent
//                                     stdout/stderr lines (KiCad's messages go there)
//   GET  /snapshot                    PNG of the app's key window
//
// Not for release builds: it opens a LAN port. See tools/corvprobe/README.md.

#import <Foundation/Foundation.h>
#import <OSLog/OSLog.h>
#import <UIKit/UIKit.h>
#import <mach/mach.h>
#import <os/proc.h>
#import <sys/socket.h>
#import <sys/sysctl.h>
#import <netinet/in.h>
#import <arpa/inet.h>
#import <dirent.h>

enum { kPort = 8765, kSamples = 18000, kLines = 2000 }; // 30 minutes of samples at 100 ms
static uint64_t gStart;
static uint64_t gTimes[kSamples], gFootprints[kSamples];
static NSUInteger gCount;
static NSLock *gLock;
static NSString *gToken;
static NSMutableArray<NSString *> *gLines; // last kLines of stdout/stderr

static uint64_t nowMs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }

static task_vm_info_data_t vmInfo(void) {
    task_vm_info_data_t info = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count);
    return info;
}

static NSString *documents(void) {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

// Resolves a request path under Documents; nil for anything that escapes it.
static NSString *safePath(NSString *relative) {
    NSString *root = documents().stringByStandardizingPath;
    NSString *full = [root stringByAppendingPathComponent:relative ?: @""].stringByStandardizingPath;
    return [full isEqualToString:root] || [full hasPrefix:[root stringByAppendingString:@"/"]] ? full : nil;
}

static unsigned long long folderSize(NSString *path) {
    unsigned long long total = 0;
    NSDirectoryEnumerator *walk = [NSFileManager.defaultManager enumeratorAtPath:path];
    while ([walk nextObject]) total += [walk.fileAttributes[NSFileSize] unsignedLongLongValue];
    return total;
}

static NSString *machine(void) {
    char name[64] = {0};
    size_t size = sizeof(name);
    sysctlbyname("hw.machine", name, &size, NULL, 0);
    return @(name);
}

static NSDictionary *metrics(void) {
    task_vm_info_data_t info = vmInfo();
    NSString *bundle = NSBundle.mainBundle.bundlePath;
    static unsigned long long bundleBytes = 0;
    if (!bundleBytes) bundleBytes = folderSize(bundle);
    return @{
        @"footprint": @(info.phys_footprint), @"footprint_peak": @(info.ledger_phys_footprint_peak),
        @"resident": @(info.resident_size), @"available": @(os_proc_available_memory()),
        @"uptime_ms": @(nowMs() - gStart), @"bundle_id": NSBundle.mainBundle.bundleIdentifier ?: @"",
        @"bundle_path": bundle, @"bundle_bytes": @(bundleBytes),
        @"executable_bytes": [NSFileManager.defaultManager attributesOfItemAtPath:NSBundle.mainBundle.executablePath error:nil][NSFileSize] ?: @0,
        @"machine": machine(), @"os": NSProcessInfo.processInfo.operatingSystemVersionString,
        @"physical_memory": @(NSProcessInfo.processInfo.physicalMemory),
        @"thermal_state": @(NSProcessInfo.processInfo.thermalState),
        @"low_power": @(NSProcessInfo.processInfo.lowPowerModeEnabled),
        @"documents": documents(),
    };
}

static NSDictionary *fonts(void) {
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    for (NSString *folder in @[@"/System/Library/Fonts", @"/System/Library/Fonts/Core", @"/System/Library/Fonts/CoreUI",
                               @"/System/Library/Fonts/LanguageSupport"]) {
        NSError *error = nil;
        NSArray *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:&error];
        NSString *sample = [names filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF ENDSWITH[c] '.ttc' OR SELF ENDSWITH[c] '.ttf'"]].firstObject;
        BOOL readable = sample && [NSFileManager.defaultManager isReadableFileAtPath:[folder stringByAppendingPathComponent:sample]];
        result[folder] = names ? @{@"entries": @(names.count), @"sample": sample ?: @"", @"sample_readable": @(readable)}
                               : @{@"error": error.localizedDescription ?: @"unreadable"};
    }
    return result;
}

static NSDictionary *timeline(uint64_t since) {
    NSMutableArray *samples = [NSMutableArray array];
    [gLock lock];
    NSUInteger first = gCount > kSamples ? gCount - kSamples : 0;
    for (NSUInteger i = first; i < gCount; ++i) {
        NSUInteger slot = i % kSamples;
        if (gTimes[slot] >= since) [samples addObject:@[@(gTimes[slot]), @(gFootprints[slot])]];
    }
    [gLock unlock];
    return @{@"interval_ms": @100, @"samples": samples};
}

static uint64_t peakSince(uint64_t since) {
    uint64_t peak = 0;
    [gLock lock];
    NSUInteger first = gCount > kSamples ? gCount - kSamples : 0;
    for (NSUInteger i = first; i < gCount; ++i)
        if (gTimes[i % kSamples] >= since && gFootprints[i % kSamples] > peak) peak = gFootprints[i % kSamples];
    [gLock unlock];
    return peak;
}

// Runs a Diagnostics.swift command on the main thread, as the app's own UI code would.
static NSDictionary *run(NSString *command, NSString *relative) {
    NSMutableDictionary *reply = [NSMutableDictionary dictionary];
    uint64_t start = nowMs() - gStart;
    uint64_t before = vmInfo().phys_footprint;
    dispatch_sync(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:@"org.corvcad.diagnostics" object:nil
                                                        userInfo:@{@"command": command, @"path": relative ?: @"", @"reply": reply}];
    });
    [NSThread sleepForTimeInterval:0.15]; // one more sample after the run
    if (reply.count == 0) reply[@"error"] = @"no reply: this build lacks CORVCAD_DIAGNOSTICS";
    reply[@"footprint_before"] = @(before);
    reply[@"footprint_after"] = @(vmInfo().phys_footprint);
    reply[@"footprint_peak_during"] = @(MAX(peakSince(start), vmInfo().phys_footprint));
    reply[@"wall_ms"] = @(nowMs() - gStart - start);
    return reply;
}

// The app's own os_log entries of the last seconds (OSLogStore, iOS 15+), newest last, plus
// the last stdout/stderr lines, which carry KiCad's and wx's messages.
static NSDictionary *recentLog(double seconds, NSUInteger limit) {
    NSMutableArray *entries = [NSMutableArray array];
    NSError *error = nil;
    OSLogStore *store = [OSLogStore storeWithScope:OSLogStoreCurrentProcessIdentifier error:&error];
    OSLogEnumerator *walk = [store entriesEnumeratorWithOptions:0
                                                       position:[store positionWithDate:[NSDate dateWithTimeIntervalSinceNow:-seconds]]
                                                      predicate:nil error:&error];
    for (OSLogEntry *entry in walk) {
        NSMutableDictionary *row = [@{@"time": @(entry.date.timeIntervalSince1970), @"message": entry.composedMessage ?: @""} mutableCopy];
        if ([entry isKindOfClass:OSLogEntryLog.class]) {
            OSLogEntryLog *log = (OSLogEntryLog *)entry;
            row[@"subsystem"] = log.subsystem ?: @"";
            row[@"category"] = log.category ?: @"";
            row[@"level"] = @(log.level);
        }
        [entries addObject:row];
    }
    if (entries.count > limit) [entries removeObjectsInRange:NSMakeRange(0, entries.count - limit)];
    [gLock lock];
    NSArray *lines = gLines ? [gLines subarrayWithRange:NSMakeRange(gLines.count > limit ? gLines.count - limit : 0, MIN(limit, gLines.count))] : @[];
    [gLock unlock];
    return @{@"os_log": entries, @"stdio": lines, @"os_log_error": store ? @"" : (error.localizedDescription ?: @"no log store")};
}

// Keeps the last kLines of stdout and stderr, still passing them on to the original stderr.
static void captureStdio(void) {
    int fds[2];
    int original = dup(STDERR_FILENO);
    if (original < 0 || pipe(fds) != 0) return;
    int reader = fds[0]; // blocks cannot capture arrays
    gLines = [NSMutableArray array];
    setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    [NSThread detachNewThreadWithBlock:^{
        NSMutableData *pending = [NSMutableData data];
        char buffer[4096];
        ssize_t n;
        while ((n = read(reader, buffer, sizeof buffer)) > 0) {
            write(original, buffer, (size_t)n);
            [pending appendBytes:buffer length:(NSUInteger)n];
            @autoreleasepool {
                for (;;) {
                    NSRange end = [pending rangeOfData:[NSData dataWithBytes:"\n" length:1] options:0 range:NSMakeRange(0, pending.length)];
                    if (end.location == NSNotFound) break;
                    NSString *line = [[NSString alloc] initWithData:[pending subdataWithRange:NSMakeRange(0, end.location)]
                                                           encoding:NSUTF8StringEncoding] ?: @"(not UTF-8)";
                    [pending replaceBytesInRange:NSMakeRange(0, end.location + 1) withBytes:NULL length:0];
                    [gLock lock];
                    [gLines addObject:line];
                    if (gLines.count > kLines) [gLines removeObjectAtIndex:0];
                    [gLock unlock];
                }
            }
        }
    }];
}

// The key window as the user sees it, drawn on the main thread.
static NSData *snapshot(void) {
    __block NSData *png = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        UIWindow *window = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
            if ([scene isKindOfClass:UIWindowScene.class])
                for (UIWindow *candidate in ((UIWindowScene *)scene).windows)
                    if (candidate.isKeyWindow) window = candidate;
        if (!window) return;
        UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithBounds:window.bounds];
        png = [renderer PNGDataWithActions:^(UIGraphicsImageRendererContext *context) {
            [window drawViewHierarchyInRect:window.bounds afterScreenUpdates:NO];
        }];
    });
    return png;
}

static void respond(int client, int status, NSString *type, NSData *body) {
    NSString *head = [NSString stringWithFormat:@"HTTP/1.1 %d %@\r\nContent-Type: %@\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
                      status, status == 200 ? @"OK" : @"Error", type, (unsigned long)body.length];
    NSMutableData *out = [[head dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
    [out appendData:body];
    for (NSUInteger sent = 0; sent < out.length;) {
        ssize_t n = send(client, (const char *)out.bytes + sent, out.length - sent, 0);
        if (n <= 0) break;
        sent += (NSUInteger)n;
    }
}

static void respondJSON(int client, int status, id object) {
    NSData *body = [NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingPrettyPrinted error:nil] ?: [NSData data];
    respond(client, status, @"application/json", body);
}

static void handle(int client) {
    NSMutableData *request = [NSMutableData data];
    char buffer[65536];
    NSRange headerEnd = NSMakeRange(NSNotFound, 0);
    while (headerEnd.location == NSNotFound) {
        ssize_t n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) { close(client); return; }
        [request appendBytes:buffer length:(NSUInteger)n];
        headerEnd = [request rangeOfData:[@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding] options:0 range:NSMakeRange(0, request.length)];
        if (request.length > 1 << 20 && headerEnd.location == NSNotFound) { close(client); return; }
    }
    NSString *header = [[NSString alloc] initWithData:[request subdataWithRange:NSMakeRange(0, headerEnd.location)] encoding:NSUTF8StringEncoding];
    NSArray *line = [[header componentsSeparatedByString:@"\r\n"].firstObject componentsSeparatedByString:@" "];
    if (line.count < 2) { close(client); return; }
    NSURLComponents *url = [NSURLComponents componentsWithString:[@"http://probe" stringByAppendingString:line[1]]];
    NSMutableDictionary *query = [NSMutableDictionary dictionary];
    for (NSURLQueryItem *item in url.queryItems) query[item.name] = item.value ?: @"";
    if (![query[@"token"] isEqualToString:gToken]) { respondJSON(client, 403, @{@"error": @"bad token"}); close(client); return; }

    NSString *route = url.path;
    if ([route isEqualToString:@"/metrics"]) respondJSON(client, 200, metrics());
    else if ([route isEqualToString:@"/timeline"]) respondJSON(client, 200, timeline([query[@"since"] longLongValue]));
    else if ([route isEqualToString:@"/fonts"]) respondJSON(client, 200, fonts());
    else if ([route isEqualToString:@"/run"]) respondJSON(client, 200, run(query[@"command"] ?: @"", query[@"path"]));
    else if ([route isEqualToString:@"/log"])
        respondJSON(client, 200, recentLog(query[@"seconds"] ? [query[@"seconds"] doubleValue] : 300, query[@"limit"] ? (NSUInteger)[query[@"limit"] integerValue] : 500));
    else if ([route isEqualToString:@"/snapshot"]) {
        NSData *png = snapshot();
        if (png) respond(client, 200, @"image/png", png);
        else respondJSON(client, 503, @{@"error": @"no key window"});
    }
    else if ([route isEqualToString:@"/ls"]) {
        NSString *path = safePath(query[@"path"]);
        NSMutableArray *entries = [NSMutableArray array];
        for (NSString *name in path ? [NSFileManager.defaultManager contentsOfDirectoryAtPath:path error:nil] : @[]) {
            NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:[path stringByAppendingPathComponent:name] error:nil];
            [entries addObject:@{@"name": name, @"bytes": attributes[NSFileSize] ?: @0,
                                 @"folder": @([attributes[NSFileType] isEqual:NSFileTypeDirectory])}];
        }
        respondJSON(client, path ? 200 : 400, path ? entries : @{@"error": @"bad path"});
    } else if ([route isEqualToString:@"/get"]) {
        NSString *path = safePath(query[@"path"]);
        NSData *data = path ? [NSData dataWithContentsOfFile:path] : nil;
        if (data) respond(client, 200, @"application/octet-stream", data);
        else respondJSON(client, 404, @{@"error": @"not found"});
    } else if ([route isEqualToString:@"/put"] && [line[0] isEqualToString:@"POST"]) {
        NSString *path = safePath(query[@"path"]);
        NSUInteger length = 0;
        for (NSString *field in [header componentsSeparatedByString:@"\r\n"])
            if ([field.lowercaseString hasPrefix:@"content-length:"]) length = (NSUInteger)[[field substringFromIndex:15] longLongValue];
        NSMutableData *body = [[request subdataWithRange:NSMakeRange(NSMaxRange(headerEnd), request.length - NSMaxRange(headerEnd))] mutableCopy];
        while (body.length < length) {
            ssize_t n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            [body appendBytes:buffer length:(NSUInteger)n];
        }
        BOOL ok = path && body.length == length &&
                  [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil] &&
                  [body writeToFile:path atomically:YES];
        respondJSON(client, ok ? 200 : 400, @{@"written": @(ok), @"bytes": @(body.length)});
    } else respondJSON(client, 404, @{@"error": @"unknown route"});
    close(client);
}

static void serve(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = {.sin_len = sizeof(address), .sin_family = AF_INET,
                                  .sin_port = htons(kPort), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(server, 8) != 0) {
        NSLog(@"[CorvProbe] cannot listen on %d: %s", kPort, strerror(errno));
        return;
    }
    NSLog(@"[CorvProbe] listening on %d", kPort);
    dispatch_queue_t queue = dispatch_queue_create("corvprobe.requests", DISPATCH_QUEUE_CONCURRENT);
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client >= 0) dispatch_async(queue, ^{ @autoreleasepool { handle(client); } });
    }
}

__attribute__((constructor)) static void start(void) {
    if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"org.corvcad.CorvCAD"]) return;
    gStart = nowMs();
    gLock = [NSLock new];
    captureStdio();
    NSString *tokenFile = [documents() stringByAppendingPathComponent:@"corvprobe.token"];
    gToken = [[NSString stringWithContentsOfFile:tokenFile encoding:NSUTF8StringEncoding error:nil]
              stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (gToken.length < 8) {
        gToken = [[NSUUID UUID].UUIDString substringToIndex:8].lowercaseString;
        [gToken writeToFile:tokenFile atomically:YES encoding:NSUTF8StringEncoding error:nil];
    }
    [NSThread detachNewThreadWithBlock:^{
        for (;;) {
            @autoreleasepool {
                uint64_t footprint = vmInfo().phys_footprint;
                [gLock lock];
                gTimes[gCount % kSamples] = nowMs() - gStart;
                gFootprints[gCount % kSamples] = footprint;
                ++gCount;
                [gLock unlock];
            }
            usleep(100000);
        }
    }];
    [NSThread detachNewThreadWithBlock:^{ serve(); }];
}

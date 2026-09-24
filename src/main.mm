#import <Cocoa/Cocoa.h>
#import <Security/Security.h>
#import <WebKit/WebKit.h>

#include <fstream>
#include <memory>
#include <string>

#include "control_server.hpp"
#include "inject.js.hpp"
#include "url_util.hpp"

static NSString* S(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
static std::string U(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }

static std::string toJSON(id obj) {
    NSData* d = [NSJSONSerialization dataWithJSONObject:obj ?: [NSNull null]
                                                options:NSJSONWritingFragmentsAllowed | NSJSONWritingSortedKeys
                                                  error:nil];
    return d ? std::string((const char*)d.bytes, d.length) : "null";
}

static NSString* isoNow() {
    static NSISO8601DateFormatter* f = [NSISO8601DateFormatter new];
    @synchronized(f) { return [f stringFromDate:[NSDate date]]; }
}

static NSString* dataDir() {
    NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@".curf"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}

static void appendLine(NSString* path, const std::string& line) {
    std::ofstream out(U(path), std::ios::app);
    out << line << '\n';
}

static NSImage* icon(NSString* name, NSString* desc) {
    NSImage* img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:desc];
    return img ?: [NSImage imageWithSystemSymbolName:@"questionmark.circle" accessibilityDescription:desc];
}

// Runs `block` on the main thread and waits for it (safe from the server threads).
static void onMain(dispatch_block_t block) {
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

@interface Browser : NSObject <NSApplicationDelegate, WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler>
@property(strong) NSWindow* window;
@property(strong) WKWebView* web;
@property(strong) NSTextField* address;
@property(strong) NSButton *backBtn, *fwdBtn, *reloadBtn, *secureBtn, *pickBtn;
@property(strong) NSProgressIndicator* progress;
@property(strong) NSMutableArray<NSDictionary*>* changes;
@property(assign) long long seq;
@property(copy) NSString* initialURL;
@end

@implementation Browser {
    std::unique_ptr<curf::ControlServer> _server;
}

#pragma mark - UI

- (NSButton*)button:(NSString*)sym tip:(NSString*)tip action:(SEL)action x:(CGFloat)x {
    NSButton* b = [NSButton buttonWithImage:icon(sym, tip) target:self action:action];
    b.bordered = NO;
    b.toolTip = tip;
    b.frame = NSMakeRect(x, 8, 30, 28);
    b.imageScaling = NSImageScaleProportionallyDown;
    b.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:15 weight:NSFontWeightMedium];
    return b;
}

- (void)buildMenu {
    NSMenu* bar = [NSMenu new];
    NSMenuItem* appItem = [NSMenuItem new];
    NSMenu* app = [NSMenu new];
    [app addItemWithTitle:@"Quit curf" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = app;
    [bar addItem:appItem];

    NSMenuItem* editItem = [NSMenuItem new];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = edit;
    [bar addItem:editItem];

    NSMenuItem* navItem = [NSMenuItem new];
    NSMenu* nav = [[NSMenu alloc] initWithTitle:@"Navigate"];
    [nav addItemWithTitle:@"Open Location" action:@selector(focusAddress:) keyEquivalent:@"l"].target = self;
    [nav addItemWithTitle:@"Reload" action:@selector(reload:) keyEquivalent:@"r"].target = self;
    [nav addItemWithTitle:@"Back" action:@selector(goBack:) keyEquivalent:@"["].target = self;
    [nav addItemWithTitle:@"Forward" action:@selector(goForward:) keyEquivalent:@"]"].target = self;
    [nav addItemWithTitle:@"Select Element" action:@selector(togglePick:) keyEquivalent:@"e"].target = self;
    [nav addItemWithTitle:@"Site Information" action:@selector(showSiteInfo:) keyEquivalent:@"i"].target = self;
    navItem.submenu = nav;
    [bar addItem:navItem];
    NSApp.mainMenu = bar;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
    self.changes = [NSMutableArray array];
    [self buildMenu];

    NSRect frame = NSMakeRect(0, 0, 1280, 860);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"curf";
    self.window.minSize = NSMakeSize(520, 320);
    NSView* root = self.window.contentView;
    const CGFloat barH = 44, W = frame.size.width, H = frame.size.height;

    NSVisualEffectView* bar = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, H - barH, W, barH)];
    bar.material = NSVisualEffectMaterialHeaderView;
    bar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [root addSubview:bar];

    self.backBtn = [self button:@"chevron.left" tip:@"Back" action:@selector(goBack:) x:8];
    self.fwdBtn = [self button:@"chevron.right" tip:@"Forward" action:@selector(goForward:) x:38];
    self.reloadBtn = [self button:@"arrow.clockwise" tip:@"Reload" action:@selector(reload:) x:70];
    self.secureBtn = [self button:@"info.circle" tip:@"Site information" action:@selector(showSiteInfo:) x:104];
    for (NSButton* b in @[ self.backBtn, self.fwdBtn, self.reloadBtn, self.secureBtn ]) [bar addSubview:b];

    self.pickBtn = [self button:@"cursorarrow.rays" tip:@"Select an element to annotate (⌘E)"
                         action:@selector(togglePick:) x:W - 40];
    [self.pickBtn setButtonType:NSButtonTypePushOnPushOff];
    self.pickBtn.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:self.pickBtn];

    self.address = [[NSTextField alloc] initWithFrame:NSMakeRect(140, 10, W - 190, 24)];
    self.address.placeholderString = @"Search Google or type a URL";
    self.address.bezelStyle = NSTextFieldRoundedBezel;
    self.address.font = [NSFont systemFontOfSize:13];
    self.address.autoresizingMask = NSViewWidthSizable;
    self.address.target = self;
    self.address.action = @selector(addressEntered:);
    self.address.cell.scrollable = YES;
    self.address.cell.wraps = NO;
    [bar addSubview:self.address];

    self.progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, W, 3)];
    self.progress.style = NSProgressIndicatorStyleBar;
    self.progress.indeterminate = NO;
    self.progress.minValue = 0;
    self.progress.maxValue = 1;
    self.progress.autoresizingMask = NSViewWidthSizable;
    self.progress.hidden = YES;
    [bar addSubview:self.progress];

    WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
    WKUserScript* script = [[WKUserScript alloc] initWithSource:S(kInjectedScript)
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentEnd
                                               forMainFrameOnly:YES];
    [cfg.userContentController addUserScript:script];
    [cfg.userContentController addScriptMessageHandler:self name:@"curf"];
    cfg.preferences.javaScriptCanOpenWindowsAutomatically = YES;
    cfg.preferences.elementFullscreenEnabled = YES;

    self.web = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, W, H - barH) configuration:cfg];
    self.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.web.navigationDelegate = self;
    self.web.UIDelegate = self;
    self.web.allowsBackForwardNavigationGestures = YES;
    self.web.allowsMagnification = YES;
    if (@available(macOS 13.3, *)) self.web.inspectable = YES;
    [root addSubview:self.web];

    for (NSString* k in @[ @"URL", @"title", @"loading", @"estimatedProgress", @"hasOnlySecureContent",
                           @"canGoBack", @"canGoForward" ])
        [self.web addObserver:self forKeyPath:k options:NSKeyValueObservingOptionNew context:nil];

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self refreshChrome];

    [self startServer];
    if (self.initialURL.length) [self navigate:self.initialURL];
    else [self.window makeFirstResponder:self.address];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { return YES; }

- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)o change:(NSDictionary*)c context:(void*)ctx {
    [self refreshChrome];
}

- (void)refreshChrome {
    WKWebView* w = self.web;
    NSText* editor = self.address.currentEditor;
    if (!editor) self.address.stringValue = w.URL.absoluteString ?: @"";
    self.window.title = w.title.length ? w.title : @"curf";
    self.backBtn.enabled = w.canGoBack;
    self.fwdBtn.enabled = w.canGoForward;
    self.reloadBtn.image = icon(w.loading ? @"xmark" : @"arrow.clockwise", w.loading ? @"Stop" : @"Reload");
    self.reloadBtn.toolTip = w.loading ? @"Stop" : @"Reload";
    self.progress.hidden = !w.loading;
    self.progress.doubleValue = w.estimatedProgress;

    NSString* scheme = w.URL.scheme.lowercaseString;
    NSString* sym = @"info.circle";
    NSColor* tint = NSColor.secondaryLabelColor;
    NSString* tip = @"Site information";
    if ([scheme isEqualToString:@"https"] && w.hasOnlySecureContent) {
        sym = @"lock.fill"; tint = NSColor.systemGreenColor; tip = @"Connection is secure";
    } else if ([scheme isEqualToString:@"https"]) {
        sym = @"lock.trianglebadge.exclamationmark"; tint = NSColor.systemOrangeColor;
        tip = @"Secure connection, but page has insecure content";
    } else if ([scheme isEqualToString:@"http"]) {
        sym = @"lock.open.fill"; tint = NSColor.systemRedColor; tip = @"Connection is not secure";
    }
    self.secureBtn.image = icon(sym, tip);
    self.secureBtn.contentTintColor = tint;
    self.secureBtn.toolTip = tip;
}

#pragma mark - Actions

- (void)navigate:(NSString*)input {
    std::string resolved = curf::resolveInput(U(input));
    if (resolved.empty()) return;
    NSURL* url = [NSURL URLWithString:S(resolved)];
    if (!url) url = [NSURL URLWithString:S("https://www.google.com/search?q=" + curf::urlEncode(U(input)))];
    [self.web loadRequest:[NSURLRequest requestWithURL:url]];
    [self.window makeFirstResponder:self.web];
}

- (void)addressEntered:(NSTextField*)f { [self navigate:f.stringValue]; }
- (void)focusAddress:(id)s { [self.window makeFirstResponder:self.address]; }
- (void)goBack:(id)s { [self.web goBack]; }
- (void)goForward:(id)s { [self.web goForward]; }
- (void)reload:(id)s {
    if (self.web.loading) [self.web stopLoading];
    else [self.web reload];
}

- (void)setPicking:(BOOL)on {
    self.pickBtn.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    self.pickBtn.contentTintColor = on ? NSColor.controlAccentColor : nil;
    [self.web evaluateJavaScript:[NSString stringWithFormat:@"window.__curf && __curf.setActive(%@)", on ? @"true" : @"false"]
               completionHandler:nil];
    if (on) [self.window makeFirstResponder:self.web];
}

- (void)togglePick:(id)sender {
    BOOL on = sender == self.pickBtn ? self.pickBtn.state == NSControlStateValueOn
                                     : self.pickBtn.state != NSControlStateValueOn;
    [self setPicking:on];
}

- (NSString*)certificateSummary {
    SecTrustRef trust = self.web.serverTrust;
    if (!trust) return @"No certificate (connection is not encrypted).";
    NSMutableString* out = [NSMutableString string];
    CFErrorRef err = nullptr;
    bool valid = SecTrustEvaluateWithError(trust, &err);
    [out appendFormat:@"Certificate: %@\n", valid ? @"valid and trusted" : @"NOT trusted"];
    if (err) CFRelease(err);
    CFArrayRef chain = SecTrustCopyCertificateChain(trust);
    if (chain) {
        CFIndex n = CFArrayGetCount(chain);
        for (CFIndex i = 0; i < n; ++i) {
            SecCertificateRef cert = (SecCertificateRef)CFArrayGetValueAtIndex(chain, i);
            NSString* name = CFBridgingRelease(SecCertificateCopySubjectSummary(cert));
            [out appendFormat:@"%@%@\n", i == 0 ? @"Issued to: " : @"  ↳ issued by: ", name ?: @"?"];
        }
        CFRelease(chain);
    }
    return out;
}

- (void)showSiteInfo:(id)sender {
    NSURL* u = self.web.URL;
    NSAlert* a = [NSAlert new];
    a.messageText = u.host ?: @"No site loaded";
    a.icon = self.secureBtn.image;
    a.informativeText = [NSString stringWithFormat:@"%@\n\nURL: %@\nProtocol: %@\nOnly secure content: %@\n\n%@",
                                                   self.secureBtn.toolTip, u.absoluteString ?: @"-",
                                                   u.scheme.uppercaseString ?: @"-",
                                                   self.web.hasOnlySecureContent ? @"yes" : @"no",
                                                   [self certificateSummary]];
    [a addButtonWithTitle:@"OK"];
    [a beginSheetModalForWindow:self.window completionHandler:nil];
}

#pragma mark - Changes & annotations

- (NSDictionary*)record:(NSString*)type data:(NSDictionary*)data {
    NSMutableDictionary* m = [NSMutableDictionary dictionaryWithDictionary:data ?: @{}];
    @synchronized(self.changes) {
        m[@"seq"] = @(++self.seq);
        m[@"type"] = type;
        m[@"time"] = isoNow();
        [self.changes addObject:m];
        if (self.changes.count > 10000) [self.changes removeObjectAtIndex:0];
    }
    appendLine([dataDir() stringByAppendingPathComponent:@"changes.jsonl"], toJSON(m));
    return m;
}

- (NSDictionary*)saveAnnotation:(NSDictionary*)element note:(NSString*)note {
    NSMutableDictionary* a = [element mutableCopy];
    [a removeObjectForKey:@"type"];
    a[@"note"] = note ?: @"";
    a[@"time"] = isoNow();
    a[@"url"] = a[@"url"] ?: self.web.URL.absoluteString ?: @"";
    a[@"title"] = self.web.title ?: @"";
    appendLine([dataDir() stringByAppendingPathComponent:@"annotations.jsonl"], toJSON(a));
    [self record:@"annotation" data:a];
    NSString* js = [NSString stringWithFormat:
        @"(()=>{const e=document.querySelector(%@);if(e){e.style.outline='2px dashed #ff2d95';e.title=%@;}})()",
        S(toJSON(a[@"selector"] ?: @"")), S(toJSON(note ?: @""))];
    [self.web evaluateJavaScript:js completionHandler:nil];
    return a;
}

- (void)promptForElement:(NSDictionary*)el {
    NSAlert* a = [NSAlert new];
    a.messageText = @"Annotate selected element";
    NSString* text = el[@"text"] ?: @"";
    if (text.length > 240) text = [[text substringToIndex:240] stringByAppendingString:@"…"];
    a.informativeText = [NSString stringWithFormat:@"<%@>  %@\n\n%@", el[@"tag"], el[@"selector"], text];
    a.icon = icon(@"cursorarrow.rays", @"Element");
    [a addButtonWithTitle:@"Save"];
    [a addButtonWithTitle:@"Cancel"];
    NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 380, 64)];
    input.placeholderString = @"Enter your note / input for this element";
    input.cell.wraps = YES;
    a.accessoryView = input;
    a.window.initialFirstResponder = input;
    [a beginSheetModalForWindow:self.window
              completionHandler:^(NSModalResponse r) {
                  if (r == NSAlertFirstButtonReturn) [self saveAnnotation:el note:input.stringValue];
              }];
}

- (void)userContentController:(WKUserContentController*)ucc didReceiveScriptMessage:(WKScriptMessage*)msg {
    if (![msg.body isKindOfClass:NSDictionary.class]) return;
    NSDictionary* body = msg.body;
    NSString* type = body[@"type"];
    if ([type isEqualToString:@"select"]) {
        [self setPicking:NO];
        [self record:@"element_selected" data:@{@"selector" : body[@"selector"] ?: @"", @"url" : body[@"url"] ?: @""}];
        [self promptForElement:body];
    } else if ([type isEqualToString:@"selectCancelled"]) {
        [self setPicking:NO];
    } else if ([type isEqualToString:@"mutations"]) {
        [self record:@"dom" data:@{@"url" : body[@"url"] ?: @"", @"items" : body[@"items"] ?: @[],
                                   @"dropped" : body[@"dropped"] ?: @0}];
    }
}

#pragma mark - Navigation delegate

- (void)webView:(WKWebView*)w didStartProvisionalNavigation:(WKNavigation*)nav {
    [self record:@"navigation_started" data:@{@"url" : w.URL.absoluteString ?: @""}];
}

- (void)webView:(WKWebView*)w didFinishNavigation:(WKNavigation*)nav {
    [self record:@"navigation_finished"
            data:@{@"url" : w.URL.absoluteString ?: @"", @"title" : w.title ?: @"", @"secure" : @(w.hasOnlySecureContent)}];
    if (self.pickBtn.state == NSControlStateValueOn) [self setPicking:YES];
}

- (void)showError:(NSError*)error inWebView:(WKWebView*)w {
    if (error.code == NSURLErrorCancelled) return;
    NSString* failing = error.userInfo[NSURLErrorFailingURLStringErrorKey] ?: @"";
    [self record:@"navigation_failed" data:@{@"url" : failing, @"error" : error.localizedDescription ?: @""}];
    NSString* html = [NSString stringWithFormat:
        @"<html><body style='font:15px -apple-system;color:#555;display:flex;align-items:center;justify-content:center;height:90vh'>"
        @"<div><h2>Can't open this page</h2><p>%@</p><p style='color:#999'>%@</p></div></body></html>",
        [[error.localizedDescription stringByReplacingOccurrencesOfString:@"<" withString:@"&lt;"] copy],
        [failing stringByReplacingOccurrencesOfString:@"<" withString:@"&lt;"]];
    [w loadHTMLString:html baseURL:nil];
}

- (void)webView:(WKWebView*)w didFailProvisionalNavigation:(WKNavigation*)nav withError:(NSError*)e {
    [self showError:e inWebView:w];
}
- (void)webView:(WKWebView*)w didFailNavigation:(WKNavigation*)nav withError:(NSError*)e {
    [self showError:e inWebView:w];
}

- (WKWebView*)webView:(WKWebView*)w createWebViewWithConfiguration:(WKWebViewConfiguration*)c
    forNavigationAction:(WKNavigationAction*)action windowFeatures:(WKWindowFeatures*)f {
    if (action.request.URL) [w loadRequest:action.request];
    return nil;
}

- (void)webView:(WKWebView*)w runJavaScriptAlertPanelWithMessage:(NSString*)message
    initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void (^)(void))done {
    NSAlert* a = [NSAlert new];
    a.messageText = message;
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) { done(); }];
}

- (void)webView:(WKWebView*)w runJavaScriptConfirmPanelWithMessage:(NSString*)message
    initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void (^)(BOOL))done {
    NSAlert* a = [NSAlert new];
    a.messageText = message;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) { done(r == NSAlertFirstButtonReturn); }];
}

- (void)webView:(WKWebView*)w runOpenPanelWithParameters:(WKOpenPanelParameters*)p
    initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void (^)(NSArray<NSURL*>*))done {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = p.allowsMultipleSelection;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
        done(r == NSModalResponseOK ? panel.URLs : nil);
    }];
}

#pragma mark - Scripting API

// Evaluates JS (may use `await`) in the page and returns {ok, value|error}.
- (NSDictionary*)runJS:(NSString*)code timeout:(double)seconds {
    __block NSDictionary* result = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    // Plain eval keeps the completion value of statements; the async forms allow top-level await
    // (as an expression, or as a function body using `return`).
    NSString* body = @"const run = () => { const ev = (0, eval); "
                     @"try { return ev(code); } catch (e) { if (!(e instanceof SyntaxError)) throw e; } "
                     @"try { return ev('(async () => (' + code + '\\n))()'); } catch (e) { if (!(e instanceof SyntaxError)) throw e; } "
                     @"return ev('(async () => {' + code + '\\n})()'); }; "
                     @"try { const v = await run(); "
                     @"return JSON.stringify({ok: true, value: v === undefined ? null : v}); } "
                     @"catch (e) { return JSON.stringify({ok: false, error: String(e && e.stack || e)}); }";
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.web callAsyncJavaScript:body arguments:@{@"code" : code} inFrame:nil
                       inContentWorld:WKContentWorld.pageWorld
                    completionHandler:^(id value, NSError* error) {
                        if (error) {
                            result = @{@"ok" : @NO, @"error" : error.localizedDescription ?: @"error"};
                        } else if ([value isKindOfClass:NSString.class]) {
                            id parsed = [NSJSONSerialization JSONObjectWithData:[value dataUsingEncoding:NSUTF8StringEncoding]
                                                                        options:NSJSONReadingFragmentsAllowed error:nil];
                            result = [parsed isKindOfClass:NSDictionary.class] ? parsed : @{@"ok" : @YES, @"value" : value};
                        } else {
                            result = @{@"ok" : @YES, @"value" : [NSNull null]};
                        }
                        dispatch_semaphore_signal(sem);
                    }];
    });
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC))) != 0)
        return @{@"ok" : @NO, @"error" : @"timeout"};
    return result;
}

- (NSDictionary*)state {
    __block NSDictionary* s;
    onMain(^{
        s = @{@"url" : self.web.URL.absoluteString ?: @"", @"title" : self.web.title ?: @"",
              @"loading" : @(self.web.loading), @"progress" : @(self.web.estimatedProgress),
              @"secure" : @(self.web.hasOnlySecureContent && [self.web.URL.scheme isEqualToString:@"https"]),
              @"canGoBack" : @(self.web.canGoBack), @"canGoForward" : @(self.web.canGoForward),
              @"picking" : @(self.pickBtn.state == NSControlStateValueOn)};
    });
    return s;
}

- (NSDictionary*)waitForLoad:(double)timeout {
    NSDate* end = [NSDate dateWithTimeIntervalSinceNow:timeout];
    [NSThread sleepForTimeInterval:0.25];
    while ([end timeIntervalSinceNow] > 0) {
        __block BOOL loading;
        onMain(^{ loading = self.web.loading; });
        if (!loading) {
            NSMutableDictionary* s = [[self state] mutableCopy];
            s[@"ok"] = @YES;
            return s;
        }
        [NSThread sleepForTimeInterval:0.1];
    }
    return @{@"ok" : @NO, @"error" : @"timeout waiting for page load"};
}

- (curf::Response)handle:(const curf::Request&)req {
    const std::string& p = req.path;
    auto q = [&](const char* k, const char* def = "") {
        auto it = req.query.find(k);
        return it == req.query.end() ? std::string(def) : it->second;
    };
    NSString* body = S(req.body);
    double timeout = std::stod(q("timeout", "30"));
    bool wait = q("wait", "1") != "0";
    auto ok = [](id obj) { return curf::Response{200, toJSON(obj)}; };
    auto js = [&](NSString* code) {
        NSDictionary* r = [self runJS:code timeout:timeout];
        return curf::Response{[r[@"ok"] boolValue] ? 200 : 500, toJSON(r)};
    };
    NSString* selector = S(toJSON(S(q("selector"))));

    if (p == "/" || p == "/help") {
        return ok(@{@"ok" : @YES, @"endpoints" : @[
            @"GET  /state", @"GET  /navigate?url=<url or search>&wait=1", @"GET  /back", @"GET  /forward",
            @"GET  /reload", @"POST /eval  (body: JavaScript, await allowed)", @"GET  /html", @"GET  /text",
            @"GET  /title", @"GET  /links", @"GET  /extract?selector=<css>&attr=<name>",
            @"GET  /click?selector=<css>", @"POST /type?selector=<css>&submit=0  (body: text)",
            @"GET  /wait?selector=<css>&timeout=30", @"GET  /pick?on=1",
            @"POST /annotate?selector=<css>  (body: note)", @"GET  /annotations",
            @"GET  /changes?since=<seq>&type=<type>", @"GET  /screenshot?path=<file.png>"
        ]});
    }
    if (p == "/state") return ok([self state]);
    if (p == "/navigate") {
        NSString* target = req.query.count("url") ? S(q("url")) : body;
        onMain(^{ [self navigate:target]; });
        return ok(wait ? [self waitForLoad:timeout] : @{@"ok" : @YES});
    }
    if (p == "/back" || p == "/forward" || p == "/reload") {
        onMain(^{
            if (p == "/back") [self.web goBack];
            else if (p == "/forward") [self.web goForward];
            else [self.web reload];
        });
        return ok(wait ? [self waitForLoad:timeout] : @{@"ok" : @YES});
    }
    if (p == "/eval") return js(body.length ? body : S(q("code")));
    if (p == "/html") return js(@"document.documentElement.outerHTML");
    if (p == "/text") return js(@"document.body ? document.body.innerText : ''");
    if (p == "/title") return js(@"document.title");
    if (p == "/links")
        return js(@"Array.from(document.querySelectorAll('a[href]')).map(a => ({text: a.innerText.trim(), href: a.href}))");
    if (p == "/extract") {
        NSString* attr = S(toJSON(S(q("attr"))));
        return js([NSString stringWithFormat:
            @"Array.from(document.querySelectorAll(%@)).map(e => { const d = __curf.describe(e); "
            @"if (%@) d.attr = e.getAttribute(%@); if (e.href) d.href = e.href; if ('value' in e) d.value = e.value; return d; })",
            selector, attr, attr]);
    }
    if (p == "/click") {
        [self record:@"script_click" data:@{@"selector" : S(q("selector"))}];
        return js([NSString stringWithFormat:
            @"(() => { const e = document.querySelector(%@); if (!e) throw new Error('no element'); "
            @"e.scrollIntoView({block: 'center'}); e.click(); return __curf.describe(e); })()", selector]);
    }
    if (p == "/type") {
        [self record:@"script_type" data:@{@"selector" : S(q("selector")), @"text" : body}];
        return js([NSString stringWithFormat:
            @"(() => { const e = document.querySelector(%@); if (!e) throw new Error('no element'); e.focus(); "
            @"const proto = e instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype; "
            @"const set = Object.getOwnPropertyDescriptor(proto, 'value'); "
            @"if (e.isContentEditable) e.textContent = %@; else if (set) set.set.call(e, %@); else e.value = %@; "
            @"e.dispatchEvent(new Event('input', {bubbles: true})); e.dispatchEvent(new Event('change', {bubbles: true})); "
            @"if (%@) { if (e.form) e.form.requestSubmit ? e.form.requestSubmit() : e.form.submit(); "
            @"else e.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true})); } "
            @"return __curf.describe(e); })()",
            selector, S(toJSON(body)), S(toJSON(body)), S(toJSON(body)), q("submit", "0") == "1" ? @"true" : @"false"]);
    }
    if (p == "/wait") {
        return js([NSString stringWithFormat:
            @"new Promise((res, rej) => { const t0 = Date.now(); (function poll() { const e = document.querySelector(%@); "
            @"if (e) return res(__curf.describe(e)); if (Date.now() - t0 > %f) return rej(new Error('timeout')); "
            @"setTimeout(poll, 100); })(); })", selector, (timeout - 0.5) * 1000]);
    }
    if (p == "/pick") {
        BOOL on = q("on", "1") != "0";
        onMain(^{ [self setPicking:on]; });
        return ok(@{@"ok" : @YES, @"picking" : @(on)});
    }
    if (p == "/annotate") {
        NSDictionary* r = [self runJS:[NSString stringWithFormat:@"__curf.describe(document.querySelector(%@))", selector]
                              timeout:timeout];
        if (![r[@"value"] isKindOfClass:NSDictionary.class])
            return {404, toJSON(@{@"ok" : @NO, @"error" : @"no element matches selector"})};
        __block NSDictionary* saved;
        onMain(^{ saved = [self saveAnnotation:r[@"value"] note:body]; });
        return ok(@{@"ok" : @YES, @"annotation" : saved});
    }
    if (p == "/annotations") {
        NSString* file = [dataDir() stringByAppendingPathComponent:@"annotations.jsonl"];
        NSString* text = [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil] ?: @"";
        NSMutableArray* list = [NSMutableArray array];
        for (NSString* line in [text componentsSeparatedByString:@"\n"]) {
            if (!line.length) continue;
            id obj = [NSJSONSerialization JSONObjectWithData:[line dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
            if (obj) [list addObject:obj];
        }
        return ok(@{@"ok" : @YES, @"file" : file, @"annotations" : list});
    }
    if (p == "/changes") {
        long long since = std::stoll(q("since", "0"));
        NSString* type = S(q("type"));
        NSMutableArray* out = [NSMutableArray array];
        long long last;
        @synchronized(self.changes) {
            for (NSDictionary* c in self.changes)
                if ([c[@"seq"] longLongValue] > since && (!type.length || [c[@"type"] isEqualToString:type]))
                    [out addObject:c];
            last = self.seq;
        }
        return ok(@{@"ok" : @YES, @"last" : @(last), @"changes" : out,
                    @"file" : [dataDir() stringByAppendingPathComponent:@"changes.jsonl"]});
    }
    if (p == "/screenshot") {
        NSString* path = req.query.count("path") ? S(q("path"))
                                                  : [dataDir() stringByAppendingPathComponent:@"screenshot.png"];
        __block NSString* err = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.web takeSnapshotWithConfiguration:nil completionHandler:^(NSImage* img, NSError* e) {
                NSBitmapImageRep* rep = img ? [NSBitmapImageRep imageRepWithData:img.TIFFRepresentation] : nil;
                NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                if (!png || ![png writeToFile:path atomically:YES]) err = e.localizedDescription ?: @"snapshot failed";
                dispatch_semaphore_signal(sem);
            }];
        });
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * NSEC_PER_SEC)));
        return err ? curf::Response{500, toJSON(@{@"ok" : @NO, @"error" : err})}
                   : ok(@{@"ok" : @YES, @"path" : path});
    }
    return {404, toJSON(@{@"ok" : @NO, @"error" : @"unknown endpoint; GET / for help"})};
}

- (void)startServer {
    int port = (int)[[NSUserDefaults standardUserDefaults] integerForKey:@"port"] ?: 9333;
    __weak Browser* weakSelf = self;
    _server = std::make_unique<curf::ControlServer>(port, [weakSelf](const curf::Request& r) {
        Browser* s = weakSelf;
        if (!s) return curf::Response{500, "{\"ok\":false}"};
        @autoreleasepool { return [s handle:r]; }
    });
    if (_server->start()) fprintf(stderr, "curf: scripting API on http://127.0.0.1:%d\n", port);
}

@end

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        Browser* browser = [Browser new];
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--port" && i + 1 < argc) {
                [[NSUserDefaults standardUserDefaults] setInteger:atoi(argv[++i]) forKey:@"port"];
            } else if (a == "-h" || a == "--help") {
                printf("usage: curf [--port N] [url-or-search]\n");
                return 0;
            } else if (a[0] != '-') {
                browser.initialURL = S(a);
            }
        }
        app.delegate = browser;
        [app run];
    }
    return 0;
}

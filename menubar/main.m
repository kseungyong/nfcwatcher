/*
 * NFCWatcher 메뉴바 앱 (얕은 컨트롤러)
 *
 * 감시/변환은 C 데몬(nfcwatcher)이 담당하고, 이 앱은 제어 UI만 제공한다:
 *   - 자동 변환 On/Off  : ~/.config/nfcwatcher/paused 플래그 생성/삭제
 *   - 감시 폴더 관리     : 설정 파일의 watch = 줄 추가/삭제 후 데몬 재적용
 *   - 폴더 즉시 변환      : nfcwatcher --scan (미리보기 후 확인)
 *   - 로그인 시 자동 시작 : LaunchAgent plist 생성/삭제
 *
 * 빌드: cc -fobjc-arc -o NFCWatcher main.m -framework Cocoa
 */
#import <Cocoa/Cocoa.h>

static NSString *const kDaemonLabel = @"co.kdsys.nfcwatcher";
static NSString *const kUILabel     = @"co.kdsys.nfcwatcher-ui";

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property (strong) NSStatusItem *statusItem;
@end

@implementation AppDelegate

/* ------------------------ 경로 헬퍼 ------------------------ */
- (NSString *)configDir { return [@"~/.config/nfcwatcher" stringByExpandingTildeInPath]; }
- (NSString *)configPath { return [[self configDir] stringByAppendingPathComponent:@"nfcwatcher.conf"]; }
- (NSString *)pauseFlag  { return [[self configDir] stringByAppendingPathComponent:@"paused"]; }
- (NSString *)logPath    { return [@"~/Library/Logs/nfcwatcher.log" stringByExpandingTildeInPath]; }
- (NSString *)uiPlist    { return [[@"~/Library/LaunchAgents" stringByExpandingTildeInPath]
                                    stringByAppendingPathComponent:[kUILabel stringByAppendingString:@".plist"]]; }

/* nfcwatcher 실행 파일 찾기: 설치 위치 우선, 없으면 앱 번들 내부 */
- (NSString *)toolPath {
    NSString *installed = [@"~/.local/bin/nfcwatcher" stringByExpandingTildeInPath];
    if ([[NSFileManager defaultManager] isExecutableFileAtPath:installed]) return installed;
    NSString *bundled = [[NSBundle mainBundle] pathForResource:@"nfcwatcher" ofType:nil];
    if (bundled) return bundled;
    return installed; /* 최후: 설치 경로로 시도 */
}

/* ------------------------ 상태 ------------------------ */
- (BOOL)isPaused { return [[NSFileManager defaultManager] fileExistsAtPath:[self pauseFlag]]; }

- (void)setPaused:(BOOL)paused {
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm createDirectoryAtPath:[self configDir] withIntermediateDirectories:YES attributes:nil error:nil];
    if (paused) [fm createFileAtPath:[self pauseFlag] contents:[NSData data] attributes:nil];
    else        [fm removeItemAtPath:[self pauseFlag] error:nil];
    [self updateIcon];
}

- (BOOL)isLoginItemEnabled { return [[NSFileManager defaultManager] fileExistsAtPath:[self uiPlist]]; }

/* ------------------------ 감시 폴더 ------------------------ */
- (NSArray<NSString *> *)watchFolders {
    NSMutableArray *out = [NSMutableArray array];
    NSString *txt = [NSString stringWithContentsOfFile:[self configPath]
                                              encoding:NSUTF8StringEncoding error:nil];
    for (NSString *raw in [txt componentsSeparatedByString:@"\n"]) {
        NSString *line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([line hasPrefix:@"#"]) continue;
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *key = [[line substringToIndex:eq.location] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (![key isEqualToString:@"watch"]) continue;
        NSString *val = [[line substringFromIndex:eq.location+1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        [out addObject:[val stringByExpandingTildeInPath]];
    }
    return out;
}

/* watch 줄만 교체하고 나머지 설정 줄은 보존 */
- (void)writeWatchFolders:(NSArray<NSString *> *)folders {
    NSString *cfg = [self configPath];
    NSString *txt = [NSString stringWithContentsOfFile:cfg encoding:NSUTF8StringEncoding error:nil];
    NSMutableArray *kept = [NSMutableArray array];
    if (txt) {
        for (NSString *raw in [txt componentsSeparatedByString:@"\n"]) {
            NSString *line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            BOOL isWatch = NO;
            NSRange eq = [line rangeOfString:@"="];
            if (![line hasPrefix:@"#"] && eq.location != NSNotFound) {
                NSString *key = [[line substringToIndex:eq.location] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                isWatch = [key isEqualToString:@"watch"];
            }
            if (!isWatch) [kept addObject:raw];
        }
    }
    NSMutableString *result = [NSMutableString string];
    for (NSString *f in folders) [result appendFormat:@"watch = %@\n", f];
    for (NSString *l in kept)     [result appendFormat:@"%@\n", l];
    [[NSFileManager defaultManager] createDirectoryAtPath:[self configDir] withIntermediateDirectories:YES attributes:nil error:nil];
    [result writeToFile:cfg atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [self reloadDaemon];
}

/* ------------------------ 외부 프로세스 ------------------------ */
- (NSString *)runTool:(NSArray<NSString *> *)args {
    NSTask *t = [[NSTask alloc] init];
    t.executableURL = [NSURL fileURLWithPath:[self toolPath]];
    t.arguments = args;
    NSPipe *pipe = [NSPipe pipe];
    t.standardOutput = pipe; t.standardError = [NSPipe pipe];
    NSError *err = nil;
    if (![t launchAndReturnError:&err]) return nil;
    NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
    [t waitUntilExit];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

/* "count=N" 줄에서 N 추출 */
- (NSInteger)parseCount:(NSString *)out {
    if (!out) return -1;
    for (NSString *line in [out componentsSeparatedByString:@"\n"])
        if ([line hasPrefix:@"count="]) return [[line substringFromIndex:6] integerValue];
    return -1;
}

- (void)reloadDaemon {
    /* launchctl kickstart -k 로 데몬을 재적용 (설정 다시 읽기) */
    NSTask *t = [[NSTask alloc] init];
    t.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
    t.arguments = @[@"kickstart", @"-k",
                    [NSString stringWithFormat:@"gui/%d/%@", getuid(), kDaemonLabel]];
    [t launchAndReturnError:nil];
}

/* ------------------------ 메뉴 액션 ------------------------ */
- (void)toggleAuto:(id)sender { [self setPaused:![self isPaused]]; }

- (void)convertFolder:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = NO; panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO;
    panel.prompt = @"변환";
    panel.message = @"NFC로 변환할 폴더를 선택하세요 (하위 폴더 포함)";
    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) return;
    NSString *folder = panel.URLs.firstObject.path;

    /* 1) 미리보기: 변환 대상 개수 */
    NSString *out = [self runTool:@[@"--scan", @"--dry-run", folder]];
    NSInteger count = [self parseCount:out];
    if (count < 0) { [self alert:@"오류" info:@"변환 대상을 확인하지 못했습니다." style:NSAlertStyleWarning]; return; }
    if (count == 0) { [self alert:@"변환 대상 없음" info:@"이 폴더에는 변환할 NFD 파일명이 없습니다." style:NSAlertStyleInformational]; return; }

    /* 2) 확인 */
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"%ld개 파일을 변환할까요?", (long)count];
    a.informativeText = [NSString stringWithFormat:@"%@\n\n파일명을 Windows 호환(NFC)으로 바꿉니다.", folder];
    [a addButtonWithTitle:@"변환"]; [a addButtonWithTitle:@"취소"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;

    /* 3) 실제 변환 */
    NSString *res = [self runTool:@[@"--scan", folder]];
    NSInteger done = [self parseCount:res];
    [self alert:@"변환 완료"
           info:[NSString stringWithFormat:@"%ld개 파일을 NFC로 변환했습니다.", (long)done]
          style:NSAlertStyleInformational];
}

- (void)addFolder:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = NO; panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO; panel.prompt = @"추가";
    panel.message = @"상시 감시할 폴더를 추가하세요";
    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) return;
    NSString *folder = panel.URLs.firstObject.path;
    NSMutableArray *folders = [[self watchFolders] mutableCopy];
    if (![folders containsObject:folder]) { [folders addObject:folder]; [self writeWatchFolders:folders]; }
}

- (void)removeFolder:(NSMenuItem *)sender {
    NSString *folder = sender.representedObject;
    NSMutableArray *folders = [[self watchFolders] mutableCopy];
    [folders removeObject:folder];
    [self writeWatchFolders:folders];
}

- (void)openLog:(id)sender  { [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:[self logPath]]]; }
- (void)openConfig:(id)sender { [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:[self configPath]]]; }

- (void)toggleLoginItem:(id)sender {
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([self isLoginItemEnabled]) {
        NSTask *t = [[NSTask alloc] init]; t.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
        t.arguments = @[@"bootout", [NSString stringWithFormat:@"gui/%d", getuid()], [self uiPlist]];
        [t launchAndReturnError:nil]; [t waitUntilExit];
        [fm removeItemAtPath:[self uiPlist] error:nil];
    } else {
        NSString *exe = [[NSBundle mainBundle] executablePath];
        NSString *plist = [NSString stringWithFormat:
            @"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\"><dict>\n"
            "  <key>Label</key><string>%@</string>\n"
            "  <key>ProgramArguments</key><array><string>%@</string></array>\n"
            "  <key>RunAtLoad</key><true/>\n"
            "  <key>ProcessType</key><string>Interactive</string>\n"
            "</dict></plist>\n", kUILabel, exe];
        [fm createDirectoryAtPath:[[self uiPlist] stringByDeletingLastPathComponent]
      withIntermediateDirectories:YES attributes:nil error:nil];
        [plist writeToFile:[self uiPlist] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        NSTask *t = [[NSTask alloc] init]; t.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
        t.arguments = @[@"bootstrap", [NSString stringWithFormat:@"gui/%d", getuid()], [self uiPlist]];
        [t launchAndReturnError:nil];
    }
}

- (void)quit:(id)sender { [NSApp terminate:nil]; }

- (void)alert:(NSString *)title info:(NSString *)info style:(NSAlertStyle)style {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = title; a.informativeText = info; a.alertStyle = style;
    [a runModal];
}

/* ------------------------ 아이콘 & 메뉴 ------------------------ */
- (void)updateIcon {
    BOOL paused = [self isPaused];
    NSString *sym = paused ? @"pause.circle" : @"character.square";
    NSImage *img = [NSImage imageWithSystemSymbolName:sym accessibilityDescription:@"NFCWatcher"];
    img.template = YES;
    self.statusItem.button.image = img;
    self.statusItem.button.toolTip = paused ? @"NFCWatcher — 자동 변환 꺼짐" : @"NFCWatcher — 자동 변환 켜짐";
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    BOOL paused = [self isPaused];

    NSMenuItem *status = [[NSMenuItem alloc] initWithTitle:
        (paused ? @"● 자동 변환: 꺼짐" : @"● 자동 변환: 켜짐") action:nil keyEquivalent:@""];
    [status setEnabled:NO];
    [menu addItem:status];

    NSMenuItem *toggle = [[NSMenuItem alloc] initWithTitle:
        (paused ? @"자동 변환 켜기" : @"자동 변환 끄기")
        action:@selector(toggleAuto:) keyEquivalent:@""];
    toggle.target = self;
    [menu addItem:toggle];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *hdr = [[NSMenuItem alloc] initWithTitle:@"감시 폴더" action:nil keyEquivalent:@""];
    [hdr setEnabled:NO];
    [menu addItem:hdr];

    NSArray *folders = [self watchFolders];
    if (folders.count == 0) {
        NSMenuItem *none = [[NSMenuItem alloc] initWithTitle:@"   (없음)" action:nil keyEquivalent:@""];
        [none setEnabled:NO]; [menu addItem:none];
    }
    for (NSString *f in folders) {
        NSString *disp = [f stringByAbbreviatingWithTildeInPath];
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:disp action:nil keyEquivalent:@""];
        /* 하위 메뉴: 제거 */
        NSMenu *sub = [[NSMenu alloc] init];
        NSMenuItem *rm = [[NSMenuItem alloc] initWithTitle:@"감시에서 제거"
                             action:@selector(removeFolder:) keyEquivalent:@""];
        rm.target = self; rm.representedObject = f;
        [sub addItem:rm];
        it.submenu = sub;
        [menu addItem:it];
    }
    NSMenuItem *add = [[NSMenuItem alloc] initWithTitle:@"＋ 폴더 추가…"
                         action:@selector(addFolder:) keyEquivalent:@""];
    add.target = self; [menu addItem:add];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *conv = [[NSMenuItem alloc] initWithTitle:@"⚡ 폴더 즉시 변환…"
                          action:@selector(convertFolder:) keyEquivalent:@""];
    conv.target = self; [menu addItem:conv];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *log = [[NSMenuItem alloc] initWithTitle:@"로그 열기" action:@selector(openLog:) keyEquivalent:@""];
    log.target = self; [menu addItem:log];
    NSMenuItem *cfg = [[NSMenuItem alloc] initWithTitle:@"설정 파일 열기" action:@selector(openConfig:) keyEquivalent:@""];
    cfg.target = self; [menu addItem:cfg];

    NSMenuItem *login = [[NSMenuItem alloc] initWithTitle:@"로그인 시 자동 시작"
                           action:@selector(toggleLoginItem:) keyEquivalent:@""];
    login.target = self; login.state = [self isLoginItemEnabled] ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:login];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"NFCWatcher 종료" action:@selector(quit:) keyEquivalent:@"q"];
    quit.target = self; [menu addItem:quit];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    [self updateIcon];
    NSMenu *menu = [[NSMenu alloc] init];
    menu.delegate = self;
    self.statusItem.menu = menu;
}
@end

int main(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory]; /* Dock 아이콘 없음 */
        AppDelegate *d = [[AppDelegate alloc] init];
        app.delegate = d;
        [app run];
    }
    return 0;
}
